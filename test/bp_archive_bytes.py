"""Byte-level helpers for tests that inspect or damage single-volume archives.

Archive format 7 (docs/specification-2.0.md) is what every CLI writes; format
6 (docs/specification-1.0.md) is still written by the GUI frontends, so the
helpers read both.  Offsets are into one archive file's bytes.
"""
import struct

VOLUME_HEADER = 32
VOLUME_TRAILER = 32
END_RECORD = 48
BLOCK_HEADER = 12


def version(archive):
    return struct.unpack_from("<H", archive, 4)[0]


def table_offset(archive):
    """Offset of the file table header."""
    if version(archive) == 7:
        end = len(archive) - VOLUME_TRAILER - END_RECORD
        return struct.unpack_from("<Q", archive, end + 16)[0]
    return struct.unpack_from("<Q", archive, len(archive) - 20)[0]


def records(archive):
    """The file table records, in archive order, as dicts with name,
    name_bytes, record (offset of the record), entry_type, mtime, flags,
    entry_offset (the entry header) and method."""
    fmt7 = version(archive) == 7
    tail = 45 if fmt7 else 43
    offset = table_offset(archive)
    count = struct.unpack_from("<I", archive, offset + 4)[0]
    pos = offset + 16
    out = []
    for _ in range(count):
        name_len = struct.unpack_from("<H", archive, pos)[0]
        name = bytes(archive[pos + 2:pos + 2 + name_len])
        fields = pos + 2 + name_len
        out.append({
            "name": name.decode("utf-8", "replace"),
            "name_bytes": name,
            "record": pos,
            "fields": fields,
            "original_size": struct.unpack_from("<Q", archive, fields)[0],
            "compressed_size": struct.unpack_from("<Q", archive, fields + 8)[0],
            "entry_type": archive[fields + 16],
            "platform": archive[fields + 17],
            "mtime": struct.unpack_from("<Q", archive, fields + 18)[0],
            "entry_offset": struct.unpack_from(
                "<Q", archive, fields + (28 if fmt7 else 26))[0],
            "method": archive[fields + (36 if fmt7 else 34)],
            "flags": struct.unpack_from("<H", archive, fields + tail - 2)[0],
        })
        pos = fields + tail
    return out


def entry_header(archive, entry_offset):
    """Fields of the entry header at entry_offset: header_size, name_len,
    method, block_size, flags, and data (offset of the first block
    header)."""
    header_size, name_len = struct.unpack_from("<HH", archive, entry_offset)
    inline = entry_offset + 4 + name_len
    if version(archive) == 7:
        method, block_size = archive[inline + 8], struct.unpack_from(
            "<I", archive, inline + 10)[0]
        flags = struct.unpack_from("<H", archive, inline + 24)[0]
    else:
        method, block_size = archive[inline + 20], struct.unpack_from(
            "<I", archive, inline + 22)[0]
        flags = struct.unpack_from("<H", archive, inline + 36)[0]
    return {
        "header_size": header_size,
        "name_len": name_len,
        "method": method,
        "block_size": block_size,
        "flags": flags,
        "data": entry_offset + header_size,
    }


def first_block_data(archive, entry_offset):
    """Offset of the first payload byte (after the first block header)."""
    return entry_header(archive, entry_offset)["data"] + BLOCK_HEADER


def find(archive, name):
    """The record of entry `name` (str or bytes), or None."""
    if isinstance(name, str):
        name = name.encode("utf-8")
    for record in records(archive):
        if record["name_bytes"] == name:
            return record
    return None


# ---- format 7 single-volume archives as data ----------------------------

import zlib


def _crc(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


def parse7(archive):
    """Parse a single-volume format 7 archive into a dict: header (the 32
    volume header bytes) and entries, each a dict with name (bytes),
    original_size, method, checksum_type, block_size, entry_type, platform,
    mtime, flags, checksum and blocks, a list of (orig_size, data) with the
    stored block bytes.  Blocks are kept as written, so build7() reproduces
    compressed entries unchanged."""
    if version(archive) != 7:
        raise ValueError("not archive format 7")
    out = {"header": bytes(archive[:VOLUME_HEADER]), "entries": []}
    for record in records(archive):
        pos = record["entry_offset"]
        header_size, name_len = struct.unpack_from("<HH", archive, pos)
        inline = pos + 4 + name_len
        e = {
            "name": bytes(archive[pos + 4:pos + 4 + name_len]),
            "original_size": struct.unpack_from("<Q", archive, inline)[0],
            "method": archive[inline + 8],
            "checksum_type": archive[inline + 9],
            "block_size": struct.unpack_from("<I", archive, inline + 10)[0],
            "entry_type": archive[inline + 14],
            "platform": archive[inline + 15],
            "mtime": struct.unpack_from("<Q", archive, inline + 16)[0],
            "flags": struct.unpack_from("<H", archive, inline + 24)[0],
            "blocks": [],
        }
        pos += header_size
        left = e["original_size"]
        while left > 0:
            comp, orig = struct.unpack_from("<II", archive, pos)
            e["blocks"].append((orig, bytes(archive[pos + 12:pos + 12 + comp])))
            pos += 12 + comp
            left -= orig
        e["checksum"] = struct.unpack_from("<I", archive, pos + 8)[0]
        out["entries"].append(e)
    return out


def stored_blocks(payload, block_size):
    """Raw blocks for a payload."""
    return [(len(payload[i:i + block_size]), bytes(payload[i:i + block_size]))
            for i in range(0, len(payload), block_size)]


def entry(name, payload=b"", entry_type=0, platform=0x01, mtime=0, flags=0,
          block_size=8192):
    """A stored entry for build7()."""
    if isinstance(name, str):
        name = name.encode("utf-8")
    return {
        "name": name, "original_size": len(payload), "method": 0,
        "checksum_type": 1, "block_size": block_size if payload else 0,
        "entry_type": entry_type, "platform": platform, "mtime": mtime,
        "flags": flags, "blocks": stored_blocks(payload, block_size),
        "checksum": _crc(payload) if payload else 0,
    }


def set_payload(e, payload):
    """Replace an entry's payload with stored blocks of its block size."""
    bs = e["block_size"] or 8192
    e.update(original_size=len(payload), method=0,
             block_size=bs if payload else 0,
             blocks=stored_blocks(payload, bs),
             checksum=_crc(payload) if payload else 0)


def build7(parsed, header=None):
    """Serialize entries into a valid single-volume format 7 archive, with
    every size, location and CRC recomputed (the payload checksum is taken
    from each entry; set_payload() and entry() compute it)."""
    hdr = bytearray(header or parsed.get("header") or bytes(VOLUME_HEADER))
    if hdr[:4] != b"BOLT":
        hdr[:4] = b"BOLT"
        struct.pack_into("<HHQ", hdr, 4, 7, 0, 0x1234567890ABCDEF)
    struct.pack_into("<H", hdr, 16, 0)
    struct.pack_into("<I", hdr, 28, _crc(hdr[:28]))
    set_id = struct.unpack_from("<Q", hdr, 8)[0]
    out = bytearray(hdr)
    table = bytearray()
    total_orig = 0
    for e in parsed["entries"]:
        name = e["name"]
        loc = len(out)
        h = bytearray(34 + len(name))
        struct.pack_into("<HH", h, 0, len(h), len(name))
        h[4:4 + len(name)] = name
        i = 4 + len(name)
        struct.pack_into("<QBBIBBQH", h, i, e["original_size"], e["method"],
                         e["checksum_type"], e["block_size"], e["entry_type"],
                         e["platform"], e["mtime"], e["flags"])
        struct.pack_into("<I", h, len(h) - 4, _crc(h[:-4]))
        out += h
        comp_total = 0
        for orig, data in e["blocks"]:
            out += struct.pack("<III", len(data), orig, _crc(data)) + data
            comp_total += 12 + len(data)
        trl = struct.pack("<QI", comp_total, e["checksum"])
        out += trl + struct.pack("<I", _crc(trl))
        table += struct.pack("<H", len(name)) + name
        table += struct.pack("<QQBBQHQBHIH", e["original_size"], comp_total,
                             e["entry_type"], e["platform"], e["mtime"], 0,
                             loc, e["method"], len(h), e["checksum"],
                             e["flags"])
        total_orig += e["original_size"]
    table_offset = len(out)
    table = struct.pack("<4sIQ", b"TBLT", len(parsed["entries"]),
                        total_orig) + table
    out += table
    end = bytearray(struct.pack("<QHHIQQII", set_id, 1, 0, _crc(table),
                                table_offset, len(table),
                                len(parsed["entries"]), 0))
    end += struct.pack("<I", _crc(end)) + b"TLOB"
    out += end
    size = len(out) + VOLUME_TRAILER
    trl = struct.pack("<QHHIQ", set_id, 0, 1, 0, size)
    out += trl + struct.pack("<I", _crc(trl)) + b"BVOL"
    return bytes(out)
