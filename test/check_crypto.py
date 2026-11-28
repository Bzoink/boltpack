#!/usr/bin/env python3
"""Compare the C crypto layer (test/unit/crypto_tool.c) with the Python model.

usage: python3 -I test/check_crypto.py PATH_TO_crypto_tool [--quick]

Known-answer files in test/reference/encryption/ (Ascon LWC KATs, the XChaCha
draft vector, RFC 7693 and RFC 9106 excerpts), random agreement with
test/bp8_crypto.py, tamper and limit cases.  Every child process has a timeout.
"""
import os, random, re, subprocess, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ascon_oracle as A
import bp8_crypto as M

REF = os.path.join(HERE, "reference", "encryption")
quick = "--quick" in sys.argv
s2_only = "--s2-only" in sys.argv     # a build without suite S1: skip every S1 case
tool = sys.argv[1]
rng = random.Random(0xB8)
total = fails = 0
skipped = []

proc = subprocess.Popen([tool], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

def ask(line, timeout=120):
    box = []
    def go():
        proc.stdin.write(line + "\n"); proc.stdin.flush()
        box.append(proc.stdout.readline().strip())
    t = threading.Thread(target=go, daemon=True)
    t.start(); t.join(timeout)
    if not box:
        proc.kill(); raise SystemExit("tool timed out on: " + line[:80])
    return box[0]

def h(b):
    return b.hex() if b else "-"

def check(name, got, want):
    global total, fails
    total += 1
    if got != want:
        fails += 1
        if fails <= 25:
            print("MISMATCH", name, "\n   C     ", str(got)[:110], "\n   model ", str(want)[:110])

def records(path):
    rec = {}
    for line in open(path):
        line = line.strip()
        if not line:
            if rec: yield rec; rec = {}
            continue
        k, _, v = line.partition("=")
        rec[k.strip()] = v.strip()
    if rec: yield rec

def hexbytes(txt):
    return bytes.fromhex(re.sub(r"[^0-9a-fA-F]", "", txt))

# ---- 0. the Python model of Ascon against the KAT files (an independent check of both) ----
n_m = 0
for r in records(os.path.join(REF, "LWC_AEAD_KAT_128_128.txt")):
    n_m += 1
    if quick and n_m > 200: break
    key, nonce, pt, ad, ct = (bytes.fromhex(r[k]) for k in ("Key", "Nonce", "PT", "AD", "CT"))
    check("model ascon aead kat %s" % r["Count"], A.aead_encrypt(key, nonce, ad, pt).hex(), ct.hex())
for r in records(os.path.join(REF, "LWC_XOF_KAT_128_512_first129.txt")):
    check("model xof128 kat %s" % r["Count"], A.xof128(bytes.fromhex(r["Msg"]), 64).hex(), r["MD"].lower())
for r in records(os.path.join(REF, "LWC_HASH_KAT_128_256_first129.txt")):
    check("model hash256 kat %s" % r["Count"], A.hash256(bytes.fromhex(r["Msg"])).hex(), r["MD"].lower())

# ---- 1. Ascon known answers (SP 800-232 LWC files) -------------------------
n_aead = 0
for r in records(os.path.join(REF, "LWC_AEAD_KAT_128_128.txt")):
    key, nonce, pt, ad, ct = (bytes.fromhex(r[k]) for k in ("Key", "Nonce", "PT", "AD", "CT"))
    n_aead += 1
    if quick and n_aead > 200: break
    check("ascon aead kat %s" % r["Count"], ask("RSEAL 2 %s %s %s %s" % (h(key), h(nonce), h(ad), h(pt))), ct.hex())
    check("ascon aead open kat %s" % r["Count"], ask("ROPEN 2 %s %s %s %s" % (h(key), h(nonce), h(ad), h(ct))), "OK " + h(pt))
for r in records(os.path.join(REF, "LWC_XOF_KAT_128_512_first129.txt")):
    msg, md = bytes.fromhex(r["Msg"]), bytes.fromhex(r["MD"])
    check("xof128 kat %s" % r["Count"], ask("HU 2 64 %s" % h(msg)), md.hex())
    for n in (1, 7, 8, 9, 16, 33):        # every shorter output is a prefix of the KAT output
        check("xof128 prefix %s/%d" % (r["Count"], n), ask("HU 2 %d %s" % (n, h(msg))), md[:n].hex())
# (Ascon-Hash256 is not used by BoltPack; its KAT file stays in the tree for 1b.)

# ---- 2. XChaCha20-Poly1305: published vector -------------------------------
if not s2_only:
    txt = open(os.path.join(REF, "xchacha-draft-03-A1.txt")).read()
    def sect(label, end):
        s = txt[txt.index(label) + len(label):]
        s = s[:s.index(end)]
        out = bytearray()
        for ln in s.splitlines():
            m = re.match(r"\s+[0-9a-f]{3}\s+((?:[0-9a-f]{2} )+)", ln)
            if m: out += bytes.fromhex(m.group(1).replace(" ", ""))
        return bytes(out)
    v_pt, v_ad, v_key = sect("Plaintext:", "AAD:"), sect("AAD:", "Key:"), sect("Key:", "IV:")
    v_iv, v_ct = sect("IV:", "32-bit fixed-common part:"), sect("Ciphertext:", "Tag:")
    v_tag = bytes.fromhex(re.search(r"Tag:\s+([0-9a-f:]+)", txt).group(1).replace(":", ""))
    assert (len(v_pt), len(v_key), len(v_iv), len(v_ct), len(v_tag)) == (114, 32, 24, 114, 16)
    check("xchacha draft A.1 (C)", ask("RSEAL 1 %s %s %s %s" % (h(v_key), h(v_iv), h(v_ad), h(v_pt))), (v_ct + v_tag).hex())
    check("xchacha draft A.1 (model)", M.xchacha_seal(v_key, v_iv, v_ad, v_pt).hex(), (v_ct + v_tag).hex())
    check("xchacha draft A.1 open", ask("ROPEN 1 %s %s %s %s" % (h(v_key), h(v_iv), h(v_ad), h(v_ct + v_tag))), "OK " + h(v_pt))

    # ---- 3. BLAKE2b (RFC 7693) and Argon2id (RFC 9106) -------------------------
    b2 = hexbytes(open(os.path.join(REF, "rfc7693-blake2b-abc.txt")).read().split("\n\n", 1)[1].split("=", 1)[1])
    check("blake2b-512(abc) RFC 7693", ask("HU 1 64 %s" % h(b"abc")), b2.hex())
    t9106 = open(os.path.join(REF, "rfc9106-argon2id-5.3.txt")).read()
    def f9106(pattern):
        return hexbytes(re.search(pattern, t9106).group(1))
    pw = f9106(r"Password\[32\]:((?:[\s0-9a-f]+?))\n\s+Salt")
    salt = f9106(r"Salt\[16\]:([0-9a-f ]+)")
    secret = f9106(r"Secret\[8\]:([0-9a-f ]+)")
    adv = f9106(r"Associated data\[12\]:([0-9a-f ]+)")
    tag9106 = hexbytes(t9106[t9106.index("Tag:") + 4:])
    assert (len(pw), len(salt), len(secret), len(adv), len(tag9106)) == (32, 16, 8, 12, 32)
    check("argon2id RFC 9106 5.3 (C, with secret and AD)", ask("ARGON %s %s %s %s 32 3 4 32" % (h(pw), h(salt), h(secret), h(adv))), tag9106.hex())
    check("argon2id RFC 9106 5.3 (model/openssl)", M.argon2id(pw, salt, 32, 3, 4, 32, secret, adv).hex(), tag9106.hex())

# ---- 3b. 50 random Argon2id inputs against openssl (and argon2-cffi when installed) ----
if not s2_only:
    for i in range(10 if quick else 50):
        lanes = rng.choice([1, 2, 3, 4]); m = rng.choice([8 * lanes, 8 * lanes + 1, 8 * lanes + 3, 64, 100, 257, 1024, 4000])
        m = max(m, 8 * lanes); t = rng.choice([1, 2, 3]); ol = rng.choice([16, 32, 64])
        pw = rng.randbytes(rng.choice([0, 1, 8, 31, 100])); salt = rng.randbytes(rng.choice([8, 16, 32]))
        want = M.argon2id(pw, salt, m, t, lanes, ol)
        check("argon2id random m=%d t=%d l=%d" % (m, t, lanes), ask("ARGON %s %s - - %d %d %d %d" % (h(pw), h(salt), m, t, lanes, ol)), want.hex())
        other = M.argon2id_cffi(pw, salt, m, t, lanes, ol)
        if other is not None: check("argon2id cffi m=%d" % m, other.hex(), want.hex())
    if M.argon2id_cffi(b"x", b"saltsalt", 8, 1, 1, 16) is None: skipped.append("argon2-cffi cross-check")

# ---- 4. random agreement: hashes -------------------------------------------
lens = [0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 1000, 5000]
if quick: lens = lens[:12]
for aead in ((2,) if s2_only else (1, 2)):
    outs = (1, 16, 24, 32, 64) if aead == 1 else (1, 8, 16, 24, 32, 64, 100, 200)
    for n in lens:
        data = rng.randbytes(n); key = rng.randbytes(32)
        for ol in outs:
            check("HU s%d n=%d ol=%d" % (aead, n, ol), ask("HU %d %d %s" % (aead, ol, h(data))), M.hu(aead, data, ol).hex())
            check("HK s%d n=%d ol=%d" % (aead, n, ol), ask("HK %d %d %s %s" % (aead, ol, h(key), h(data))), M.hk(aead, key, data, ol).hex())
if not s2_only: check("HU S1 outlen 65 refused", ask("HU 1 65 -"), "ERR -1")
check("HU unknown suite", ask("HU 3 16 -"), "ERR -2")

# ---- 5. random agreement: raw AEAD, both suites ----------------------------
sizes = list(range(0, 70)) + [127, 128, 129, 255, 1000, 4096, 65535]
if quick: sizes = sizes[:40]
for aead in ((2,) if s2_only else (1, 2)):
    for n in sizes:
        key = rng.randbytes(32)
        nonce = rng.randbytes(M.NONCE_LEN[aead]); ad = rng.randbytes(rng.choice([0, 1, 15, 16, 17, 33, 89])); pt = rng.randbytes(n)
        want = M.raw_seal(aead, key, nonce, ad, pt)
        check("RSEAL s%d n=%d" % (aead, n), ask("RSEAL %d %s %s %s %s" % (aead, h(key), h(nonce), h(ad), h(pt))), want.hex())
        check("ROPEN s%d n=%d" % (aead, n), ask("ROPEN %d %s %s %s %s" % (aead, h(key), h(nonce), h(ad), h(want))), "OK " + h(pt))
        if n < 40:
            for i in range(len(want)):
                bad = bytearray(want); bad[i] ^= 1
                check("raw tamper s%d n=%d i=%d" % (aead, n, i), ask("ROPEN %d %s %s %s %s" % (aead, h(key), h(nonce), h(ad), h(bytes(bad)))), "FAIL")

# ---- 6. key header prefix, key derivation, limits ----------------------------
def derive_case(name, kdf, aead, norm, m, t, lanes, secret_len=None, expect=None, lim=""):
    salt = rng.randbytes(16); set8 = rng.randbytes(8)
    secret = rng.randbytes(32 if kdf == 0 else rng.choice([1, 5, 12, 40]) ) if secret_len is None else rng.randbytes(secret_len)
    got = ask("DERIVE %d %d %d %d %d %d %s %s %s %s" % (kdf, aead, norm, m, t, lanes, h(salt), h(set8), h(secret), lim))
    if expect is not None:
        check(name, got, "ERR %d" % expect)
        return
    _, ks, kn, ver = M.derive(kdf, aead, norm, m, t, lanes, salt, set8, secret)
    check(name, got, "OK %s %s %s" % (ks.hex(), kn.hex(), ver.hex()))
    check(name + " pack32", ask("PACK %d %d %d %d %d %d %s" % (kdf, aead, norm, m, t, lanes, h(salt))),
          M.pack32(kdf, aead, norm, m, t, lanes, salt).hex())

for i in range(6 if quick else 20):
    if not s2_only: derive_case("derive kdf0 S1", 0, 1, 0, 0, 0, 0)
    derive_case("derive kdf0 S2", 0, 2, 0, 0, 0, 0)
costs = [(8, 1, 1), (16, 1, 2), (10, 1, 1), (24, 2, 2), (64, 3, 2), (1024, 2, 1), (19456, 2, 1), (65536, 3, 1)]
if quick: costs = costs[:5]
for (m, t, l) in ([] if s2_only else costs):
    derive_case("derive argon2id m=%d t=%d l=%d" % (m, t, l), 1, 1, 1, m, t, l)
derive_case("secret 31 bytes with kdf0", 0, 2, 0, 0, 0, 0, secret_len=31, expect=-1)
derive_case("secret 33 bytes with kdf0", 0, 2, 0, 0, 0, 0, secret_len=33, expect=-1)
derive_case("kdf0 with nonzero m", 0, 2, 0, 8, 0, 0, expect=-1)
derive_case("kdf0 with nonzero t", 0, 2, 0, 0, 1, 0, expect=-1)
derive_case("kdf0 with nonzero lanes", 0, 2, 0, 0, 0, 1, expect=-1)
derive_case("kdf0 with norm 1", 0, 2, 1, 0, 0, 0, expect=-1)
if not s2_only: derive_case("argon2id with norm 0", 1, 1, 0, 64, 3, 1, expect=-1)
derive_case("argon2id with S2 (undefined pair)", 1, 2, 1, 64, 3, 1, expect=-2)
derive_case("kdf 2 (unknown)", 2, 1, 0, 0, 0, 0, expect=-2)
derive_case("aead 0", 0, 0, 0, 0, 0, 0, expect=-2)
derive_case("aead 3", 0, 3, 0, 0, 0, 0, expect=-2)
if not s2_only: derive_case("m < 8*lanes", 1, 1, 1, 15, 1, 2, expect=-1)
if not s2_only: derive_case("m = 0", 1, 1, 1, 0, 1, 1, expect=-1)
if not s2_only: derive_case("t = 0", 1, 1, 1, 64, 0, 1, expect=-1)
if not s2_only: derive_case("lanes = 0", 1, 1, 1, 64, 1, 0, expect=-1)
if not s2_only: derive_case("m above the default cap (256 MiB + 1 KiB)", 1, 1, 1, 262145, 3, 1, expect=-7)
if not s2_only: derive_case("m = 4 TiB-ish (u32 max)", 1, 1, 1, 4294967295, 3, 1, expect=-7)
if not s2_only: derive_case("t above the cap", 1, 1, 1, 64, 9, 1, expect=-7)
if not s2_only: derive_case("lanes above the cap", 1, 1, 1, 64, 1, 5, expect=-7)

# ---- 7. frame seal/open with scripted E and counter -------------------------
def env_for(aead, kind, fflags, ptlen):
    return M.envelope(kind, fflags, aead, ptlen)

extras = {1: b"", 2: b"E" * 16 + b"\x03\0\0\0", 3: b"F" * 16 + b"\x09\0\0\0", 4: b"\x01\0\0\0\x05\0\0\0", 5: b"\xAB" * 48}
sizes = [0, 1, 2, 15, 16, 17, 31, 32, 33, 100, 1000, 65535]
if quick: sizes = sizes[:8]
for aead in ((2,) if s2_only else (1, 2)):
    set8 = rng.randbytes(8); k_seal = rng.randbytes(32); k_nonce = rng.randbytes(32)
    for kind in (1, 2, 3, 4, 5):
        for n in sizes:
            if kind == 5: n = 0
            pt = rng.randbytes(n); E = rng.randbytes(32); ctr = rng.choice([0, 1, 2**32 - 1, 2**32, 2**40 + 7])
            fflags = 1 if kind == 2 and rng.random() < 0.5 else 0
            env = env_for(aead, kind, fflags, n); extra = extras[kind]
            nonce, ct, tag = M.seal_frame(aead, set8, k_seal, k_nonce, ctr, E, env, extra, pt)
            got = ask("SEAL %d %s %s %s %d %d %s %s %s %s" % (aead, h(set8), h(k_seal), h(k_nonce), ctr & 0xFFFFFFFF, ctr >> 32, h(E), h(env), h(extra), h(pt)))
            check("seal s%d kind%d n=%d" % (aead, kind, n), got, "OK %s %s %s" % (h(nonce), h(ct), h(tag)))
            check("open s%d kind%d n=%d" % (aead, kind, n), ask("OPEN %d %s %s %s %s %s %s %s" % (aead, h(set8), h(k_seal), h(env), h(extra), h(nonce), h(ct), h(tag))), "OK " + h(pt))
            if n <= 33:   # tamper every field the AAD or the tag covers
                def op(set8=set8, env=env, extra=extra, nonce=nonce, ct=ct, tag=tag):
                    return ask("OPEN %d %s %s %s %s %s %s %s" % (aead, h(set8), h(k_seal), h(env), h(extra), h(nonce), h(ct), h(tag)))
                for i in range(len(ct)):
                    b = bytearray(ct); b[i] ^= 1; check("tamper ct", op(ct=bytes(b)), "ERR -3")
                for i in range(16):
                    b = bytearray(tag); b[i] ^= 1; check("tamper tag", op(tag=bytes(b)), "ERR -3")
                for i in range(len(nonce)):
                    b = bytearray(nonce); b[i] ^= 1; check("tamper nonce", op(nonce=bytes(b)), "ERR -3")
                for i in range(8):
                    if i in (2, 3): continue
                    b = bytearray(env); b[i] ^= 1
                    if i >= 4: check("envelope len change", op(env=bytes(b)), "ERR -1")      # no longer matches the body length
                    else: check("tamper envelope kind/fflags", op(env=bytes(b)), "ERR -3")    # AAD mismatch
                for i in (2, 3):
                    b = bytearray(env); b[i] = 1; check("nonzero envelope zero byte", op(env=bytes(b)), "ERR -1")
                for i in range(len(extra)):
                    b = bytearray(extra); b[i] ^= 1; check("tamper extra", op(extra=bytes(b)), "ERR -3")
                for i in range(8):
                    b = bytearray(set8); b[i] ^= 1; check("tamper set id", op(set8=bytes(b)), "ERR -3")
# the suite id is in the AAD: a frame sealed under S1 cannot open as S2 even with the same bytes
check("seal with ctr/E scripted, wrong envelope length", ask("SEAL 2 %s %s %s 0 0 %s %s - %s" % (h(bytes(8)), h(bytes(32)), h(bytes(32)), h(bytes(32)), h(M.envelope(2, 0, 2, 5)), h(b"abc"))), "ERR -1")

# ---- 8. draft-literal AAD fixtures (hand-assembled from the text of draft 4.2 and 7) ----
def draft_aad_hex(aead, set8_hex, kind, fflags, nl, ptlen, extra_hex):
    """"bp8/aad\\0" || aead || set_id || envelope || extra, as hex pieces: the envelope is kind,
    fflags, two zero bytes and len = nl + plaintext_length + 16 as a little-endian u32.
    For the end record (kind 5) the plaintext is empty, so len = nl + 16."""
    ln = nl + ptlen + 16
    env_hex = "%02x%02x0000%02x%02x%02x%02x" % (kind, fflags, ln & 255, (ln >> 8) & 255, (ln >> 16) & 255, ln >> 24)
    return "6270382f616164" + "00" + "%02x" % aead + set8_hex + env_hex + extra_hex
LIT = [  # (aead, kind, fflags, ptlen, extra_hex, literal AAD hex written out by hand from the draft)
    (1, 1, 0, 40, "", "6270382f616164" + "00" + "01" + "0102030405060708" + "01000000" + "50000000"),
    (2, 2, 1, 100, "e" * 32 + "03000000", "6270382f616164" + "00" + "02" + "0102030405060708" + "02010000" + "84000000" + "e" * 32 + "03000000"),
    (1, 3, 0, 16, "f" * 32 + "09000000", "6270382f616164" + "00" + "01" + "0102030405060708" + "03000000" + "38000000" + "f" * 32 + "09000000"),
    (2, 4, 0, 1000, "01000000" + "05000000", "6270382f616164" + "00" + "02" + "0102030405060708" + "04000000" + "08040000" + "01000000" + "05000000"),
    (2, 5, 0, 0, "ab" * 48, "6270382f616164" + "00" + "02" + "0102030405060708" + "05000000" + "20000000" + "ab" * 48),   # end record, S2: len = 16 + 16
    (1, 5, 0, 0, "ab" * 48, "6270382f616164" + "00" + "01" + "0102030405060708" + "05000000" + "28000000" + "ab" * 48),   # end record, S1: len = 24 + 16
]
for (aead, kind, fflags, ptlen, extra_hex, literal) in LIT:
    if aead == 1 and s2_only: continue
    nl = M.NONCE_LEN[aead]
    assert literal == draft_aad_hex(aead, "0102030405060708", kind, fflags, nl, ptlen, extra_hex), (aead, kind)   # the fixture agrees with its own recipe
    env = bytes.fromhex("%02x%02x0000" % (kind, fflags)) + (nl + ptlen + 16).to_bytes(4, "little")
    check("AAD literal s%d kind%d (model)" % (aead, kind), M.aad(aead, bytes.fromhex("0102030405060708"), env, bytes.fromhex(extra_hex)).hex(), literal)
    check("AAD literal s%d kind%d (C)" % (aead, kind), ask("AAD %d 0102030405060708 %s %s" % (aead, env.hex(), extra_hex or "-")), literal)

# ---- 9. XOF: several squeezes and chunked absorbs equal one squeeze of the whole ----
for n1 in (0, 1, 7, 8, 9, 31, 32, 33, 64):
    for n2 in (1, 7, 8, 9, 32, 33):
        data = rng.randbytes(rng.choice([0, 1, 7, 8, 9, 16, 100]))
        want = A.xof128(data, n1 + n2).hex()
        check("xof 2 squeezes %d+%d" % (n1, n2), ask("XOF2 %s %d %d" % (h(data), n1, n2)), want if n1 + n2 else "-")
for ch in (1, 3, 7, 8, 9, 16, 17):
    for dl in (0, 1, 7, 8, 9, 63, 64, 65, 300):
        data = rng.randbytes(dl)
        check("xof chunked absorb %d/%d" % (ch, dl), ask("XOFC %d 40 %s" % (ch, h(data))), A.xof128(data, 40).hex())

# ---- 10. the entropy pool against its model (no OS source, imported bytes) ----
for imp in (16, 17, 32, 100):
    for draws in ([32], [32, 32], [1, 7, 8, 9], [64, 0, 5], [100, 3, 3, 3]):
        data = rng.randbytes(imp)
        want = M.pool_model(data, draws)
        check("pool import=%d draws=%s" % (imp, draws), ask("POOL %s %s" % (h(data), " ".join(map(str, draws)))), h(want))
check("pool refuses below 16 imported bytes", ask("POOL %s 32" % h(bytes(15))), "ERR")

# ---- 11. golden file: pinned inputs and outputs, checked against the C and the model ----
import json
GOLD = os.path.join(REF, "golden-crypto-v1.json")
def golden_cases():
    cases = []
    set8 = bytes.fromhex("0102030405060708"); salt = bytes(range(0x20, 0x30))
    key = bytes(range(0x40, 0x60))
    for (name, kdf, aead, norm, m, t, lanes, secret) in (
            ("kdf0-s1", 0, 1, 0, 0, 0, 0, key), ("kdf0-s2", 0, 2, 0, 0, 0, 0, key),
            ("argon2id-s1", 1, 1, 1, 64, 2, 2, "correct horse battery staple".encode())):
        _, ks, kn, ver = M.derive(kdf, aead, norm, m, t, lanes, salt, set8, secret)
        cases.append(dict(kind="derive", name=name, kdf=kdf, aead=aead, norm=norm, m=m, t=t, lanes=lanes, salt=salt.hex(),
                          set8=set8.hex(), secret=secret.hex(), pack32=M.pack32(kdf, aead, norm, m, t, lanes, salt).hex(),
                          k_seal=ks.hex(), k_nonce=kn.hex(), verifier=ver.hex()))
    for aead in (1, 2):
        ks = bytes(range(0x60, 0x80)); kn = bytes(range(0x80, 0xA0)); E = bytes(range(0xA0, 0xC0))
        for kind, fflags, n, extra in ((1, 0, 45, b""), (2, 1, 100, b"\x11" * 16 + b"\x02\0\0\0"), (3, 0, 16, b"\x11" * 16 + b"\x03\0\0\0"),
                                       (4, 0, 70, b"\0\0\0\0\x02\0\0\0"), (5, 0, 0, bytes(range(48)))):
            pt = bytes((i * 7 + kind) & 255 for i in range(n)); ctr = 7 + kind
            env = M.envelope(kind, fflags, aead, n)
            nonce, ct, tag = M.seal_frame(aead, set8, ks, kn, ctr, E, env, extra, pt)
            cases.append(dict(kind="frame", name="s%d-kind%d" % (aead, kind), aead=aead, set8=set8.hex(), k_seal=ks.hex(), k_nonce=kn.hex(),
                              ctr=ctr, E=E.hex(), envelope=env.hex(), extra=extra.hex(), pt=pt.hex(),
                              aad=M.aad(aead, set8, env, extra).hex(), nonce=nonce.hex(), ct=ct.hex(), tag=tag.hex()))
    return cases
if "--write-golden" in sys.argv:
    json.dump(dict(version=1, note="pinned by test/check_crypto.py --write-golden from test/bp8_crypto.py; checked against the C", cases=golden_cases()), open(GOLD, "w"), indent=1)
    print("wrote", GOLD)
gold = json.load(open(GOLD))
check("golden regenerates identically from the model", json.dumps(gold["cases"], sort_keys=True), json.dumps(golden_cases(), sort_keys=True))
for c in gold["cases"]:
    if c["aead"] == 1 and s2_only: continue
    if c["kind"] == "derive":
        got = ask("DERIVE %d %d %d %d %d %d %s %s %s" % (c["kdf"], c["aead"], c["norm"], c["m"], c["t"], c["lanes"], c["salt"], c["set8"], c["secret"]))
        check("golden %s" % c["name"], got, "OK %s %s %s" % (c["k_seal"], c["k_nonce"], c["verifier"]))
        check("golden %s pack32" % c["name"], ask("PACK %d %d %d %d %d %d %s" % (c["kdf"], c["aead"], c["norm"], c["m"], c["t"], c["lanes"], c["salt"])), c["pack32"])
    else:
        got = ask("SEAL %d %s %s %s %d 0 %s %s %s %s" % (c["aead"], c["set8"], c["k_seal"], c["k_nonce"], c["ctr"], c["E"], c["envelope"], c["extra"] or "-", c["pt"] or "-"))
        check("golden %s" % c["name"], got, "OK %s %s %s" % (c["nonce"], c["ct"] or "-", c["tag"]))
        check("golden %s aad" % c["name"], ask("AAD %d %s %s %s" % (c["aead"], c["set8"], c["envelope"], c["extra"] or "-")), c["aad"])

proc.stdin.close()
try:
    proc.wait(timeout=30)
except subprocess.TimeoutExpired:
    proc.kill()
print("%d checks, %d failed%s" % (total, fails, ("; skipped: " + ", ".join(skipped)) if skipped else ""))
sys.exit(1 if fails else 0)
