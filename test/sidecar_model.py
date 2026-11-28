"""Independent model of the boltpack-recover `.bpr` sidecar, version 1.

Written from the format description alone (the recovery plan, section 19), with
no code shared with the tool: its own GF(2^8), CRC (zlib), hash, grouping and
file layout.  `protect` output must equal `build()` byte for byte, and the tests
use `parse()` and the group arithmetic to plan damage and to judge repairs.
"""
import struct
import zlib

MASK64 = (1 << 64) - 1
CHUNK = 1024

# --- GF(2^8), polynomial 0x11D, generator 2 ----------------------------------
EXP = [0] * 512
LOG = [0] * 256
_x = 1
for _i in range(255):
    EXP[_i] = _x
    LOG[_x] = _i
    _x <<= 1
    if _x & 0x100:
        _x ^= 0x11D
for _i in range(255, 512):
    EXP[_i] = EXP[_i - 255]


def gmul(a, b):
    return 0 if a == 0 or b == 0 else EXP[LOG[a] + LOG[b]]


def ginv(a):
    return EXP[255 - LOG[a]]


_TABLES = {}


def mul_table(c):
    t = _TABLES.get(c)
    if t is None:
        t = bytes(gmul(c, x) for x in range(256))
        _TABLES[c] = t
    return t


def cauchy(r, j, k):
    return ginv(j ^ (r + k))


# --- parameters ----------------------------------------------------------------
def permille_of(percent):
    return int(round(float(percent) * 10))


def params(S, permille):
    """(ngroups, G, r) for S data sectors."""
    gmax = 1
    for g in range(1, 256):
        if g + max(1, -(-permille * g // 1000)) <= 255:
            gmax = g
    ngroups = -(-S // gmax)
    G = -(-S // ngroups)
    r = max(1, -(-permille * G // 1000))
    return ngroups, G, r


def group_of(i, ngroups):
    return i % ngroups


def member_of(i, ngroups):
    return i // ngroups


def group_members(g, S, ngroups):
    """Global data-sector indexes of group g, in member order."""
    return list(range(g, S, ngroups))


# --- checksums -----------------------------------------------------------------
def sector_hash(idx, data, ss):
    h = 0x243F6A8885A308D3 ^ ((idx * 0x9E3779B97F4A7C15) & MASK64)
    for off in range(0, len(data), 8):
        w = int.from_bytes(data[off:off + 8], "little")
        h = ((h ^ w) * 0xFF51AFD7ED558CCD) & MASK64
        h ^= h >> 32
    h ^= ss
    h ^= h >> 29
    h = (h * 0xC4CEB9FE1A85EC53) & MASK64
    h ^= h >> 32
    return h


def sector_entry(idx, data, ss):
    return struct.pack("<IQ", zlib.crc32(data) & 0xFFFFFFFF, sector_hash(idx, data, ss))


# --- building ------------------------------------------------------------------
def cut(volume_bytes, ss):
    """Sectors of one volume, the last zero-padded."""
    out = []
    for off in range(0, len(volume_bytes), ss):
        s = volume_bytes[off:off + ss]
        out.append(s + b"\0" * (ss - len(s)))
    return out


def encode_group(sectors, r, ss):
    """Parity sectors for one group (list of bytes, member order)."""
    parity = []
    for j in range(r):
        acc = 0
        for k, s in enumerate(sectors):
            c = cauchy(r, j, k)
            acc ^= int.from_bytes(s.translate(mul_table(c)), "little")
        parity.append(acc.to_bytes(ss, "little"))
    return parity


def build(volumes, ss, percent, set_id=0, force=None):
    """volumes: list of (base_name, bytes).  Returns (sidecar_bytes, info).
    force=(ngroups, r) writes a structurally consistent sidecar with those
    numbers and zero parity (for hostile-input tests; the numbers may be
    ones the format forbids)."""
    permille = permille_of(percent)
    secs = []
    for _, data in volumes:
        secs.extend(cut(data, ss))
    S = len(secs)
    ngroups, G, r = params(S, permille)
    if force is not None:
        ngroups, r = force
    nparity = ngroups * r
    parity = [None] * nparity
    for g in range(ngroups):
        if force is not None:
            for j in range(r):
                parity[g * r + j] = b"\0" * ss
            continue
        members = [secs[i] for i in group_members(g, S, ngroups)]
        for j, p in enumerate(encode_group(members, r, ss)):
            parity[g * r + j] = p
    entries = [sector_entry(i, secs[i], ss) for i in range(S)]
    entries += [sector_entry(S + p, parity[p], ss) for p in range(nparity)]
    table = b""
    for c in range(0, len(entries), CHUNK):
        chunk = b"".join(entries[c:c + CHUNK])
        table += chunk + struct.pack("<I", zlib.crc32(chunk) & 0xFFFFFFFF)
    vl = b""
    for name, data in volumes:
        nb = name.encode("utf-8")
        vl += struct.pack("<QH", len(data), len(nb)) + nb
    hdr_len = 80 + len(vl) + 4
    t_off = hdr_len
    p_off = t_off + len(table)
    h2_off = p_off + nparity * ss
    t2_off = h2_off + hdr_len
    total = t2_off + len(table) + 48
    fixed = b"BPRC" + struct.pack("<HHQ", 1, 0, set_id) + struct.pack(
        "<IIIIII", len(volumes), ss, S, r, ngroups, nparity) + struct.pack(
        "<QQQQ", t_off, p_off, h2_off, t2_off) + struct.pack("<II", len(vl), 0)
    assert len(fixed) == 80, len(fixed)
    body = fixed + vl
    hdr = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
    foot = b"CRPB" + struct.pack("<IQQQQ", hdr_len, h2_off, t2_off, len(table), total)
    foot += struct.pack("<II", zlib.crc32(foot) & 0xFFFFFFFF, 0)
    assert len(foot) == 48
    blob = hdr + table + b"".join(parity) + hdr + table + foot
    assert len(blob) == total
    info = dict(S=S, ngroups=ngroups, G=G, r=r, nparity=nparity, ss=ss,
                t_off=t_off, p_off=p_off, h2_off=h2_off, t2_off=t2_off,
                hdr_len=hdr_len, table_len=len(table), total=total,
                sectors=secs, parity=parity,
                vol_sectors=[len(cut(d, ss)) for _, d in volumes])
    return blob, info


# --- parsing (for tests) -------------------------------------------------------
def parse_header(blob, off=0):
    if blob[off:off + 4] != b"BPRC":
        return None
    (ver, flags, set_id) = struct.unpack_from("<HHQ", blob, off + 4)
    (nvol, ss, S, r, ngroups, nparity) = struct.unpack_from("<IIIIII", blob, off + 16)
    (t_off, p_off, h2_off, t2_off) = struct.unpack_from("<QQQQ", blob, off + 40)
    (vl_len, _) = struct.unpack_from("<II", blob, off + 72)
    end = off + 80 + vl_len
    if end + 4 > len(blob):
        return None
    if zlib.crc32(blob[off:end]) & 0xFFFFFFFF != struct.unpack_from("<I", blob, end)[0]:
        return None
    vols, p = [], off + 80
    for _ in range(nvol):
        size, nl = struct.unpack_from("<QH", blob, p)
        vols.append((blob[p + 10:p + 10 + nl].decode("utf-8"), size))
        p += 10 + nl
    return dict(version=ver, set_id=set_id, nvol=nvol, ss=ss, S=S, r=r,
                ngroups=ngroups, nparity=nparity, t_off=t_off, p_off=p_off,
                h2_off=h2_off, t2_off=t2_off, vols=vols, hdr_len=end + 4 - off)


def table_entries(blob, h, off=None):
    """Entries (crc, hash) of the table at off; chunks whose CRC fails -> None."""
    off = h["t_off"] if off is None else off
    n = h["S"] + h["nparity"]
    out, p = [], off
    for c in range(0, n, CHUNK):
        cnt = min(CHUNK, n - c)
        chunk = blob[p:p + cnt * 12]
        ok = (zlib.crc32(chunk) & 0xFFFFFFFF ==
              struct.unpack_from("<I", blob, p + cnt * 12)[0])
        for e in range(cnt):
            out.append(struct.unpack_from("<IQ", chunk, e * 12) if ok else None)
        p += cnt * 12 + 4
    return out


def patch_entry(blob, h, idx, entry):
    """Replace table entry idx (crc, hash packed) in both table copies and fix
    the chunk checksums, so the sidecar is internally consistent."""
    b = bytearray(blob)
    n = h["S"] + h["nparity"]
    for off in (h["t_off"], h["t2_off"]):
        c = idx // CHUNK
        start = off + c * (CHUNK * 12 + 4)
        cnt = min(CHUNK, n - c * CHUNK)
        e = start + (idx - c * CHUNK) * 12
        b[e:e + 12] = entry
        b[start + cnt * 12:start + cnt * 12 + 4] = struct.pack(
            "<I", zlib.crc32(bytes(b[start:start + cnt * 12])) & 0xFFFFFFFF)
    return bytes(b)
