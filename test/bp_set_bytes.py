"""Ground-truth parser for complete format 7 volume sets.

This is the oracle for the boltpack-recover tests.  It reads an *intact* set
the way the specification (docs/specification-2.0.md, sections 2 and 5) says a
reader does: end record from the last volume, file table from the end record,
then every entry by its table location, following the placement rule (2.1) for
blocks and trailers.  It shares no code with the C tool and never looks at
what the tool reports; damage is applied afterwards and the expected result is
derived from this structure map, not from any scan.

Structure map: every structure is a Structure(kind, volume, start, length,
entry, block), with offsets into that volume's file.
"""
import struct
import zlib

import bp_archive_bytes as bab

VOLUME_HEADER = 32
VOLUME_TRAILER = 32
END_RECORD = 48
ENTRY_FIXED = 34

KIND_VOLUME_HEADER = "volume-header"
KIND_VOLUME_TRAILER = "volume-trailer"
KIND_ENTRY_HEADER = "entry-header"
KIND_BLOCK = "block"
KIND_ENTRY_TRAILER = "entry-trailer"
KIND_TABLE = "table"
KIND_END_RECORD = "end-record"


def crc32(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


class Structure(object):
    __slots__ = ("kind", "volume", "start", "length", "entry", "block")

    def __init__(self, kind, volume, start, length, entry=None, block=None):
        self.kind = kind
        self.volume = volume
        self.start = start
        self.length = length
        self.entry = entry
        self.block = block

    @property
    def end(self):
        return self.start + self.length

    def __repr__(self):
        return "Structure(%s v%d %d+%d e=%s b=%s)" % (
            self.kind, self.volume, self.start, self.length, self.entry,
            self.block)


class Entry(object):
    """One archive entry with its location and every structure it owns."""

    def __init__(self):
        self.index = 0
        self.name = b""
        self.entry_type = 0
        self.platform = 0
        self.method = 0
        self.original_size = 0
        self.compressed_size = 0
        self.block_size = 0
        self.flags = 0
        self.mtime = 0
        self.checksum = 0
        self.header_size = 0
        self.volume = 0          # volume holding the entry header
        self.offset = 0          # offset of the entry header in that volume
        self.end_volume = 0      # volume holding the entry trailer
        self.end_offset = 0      # offset just past the entry trailer
        self.blocks = []         # (orig_size, comp_size)
        self.structures = []     # header, blocks, trailer


class VolumeSet(object):
    def __init__(self):
        self.paths = {}          # volume number -> path
        self.data = {}           # volume number -> bytes
        self.set_id = 0
        self.volume_count = 0
        self.entries = []
        self.table_volume = 0
        self.table_offset = 0
        self.table_size = 0
        self.table_crc = 0
        self.structures = []     # every structure, in archive order
        self.table_structures = []
        self.end_record = None

    def body_end(self, volume):
        return len(self.data[volume]) - VOLUME_TRAILER

    def name_set(self):
        return set(e.name for e in self.entries)

    # --- coverage helpers used by the fixture generator ------------------

    def has_short_end_of_volume_block(self):
        """A block shorter than its entry's block_size that is not the last
        block of the entry, or that is the first block of a later volume
        (the two shapes of an end-of-volume block, spec 2.3.2)."""
        for e in self.entries:
            for i, (orig, _comp) in enumerate(e.blocks):
                if orig < e.block_size and i + 1 < len(e.blocks):
                    return True
        return False

    def entries_crossing_volumes(self):
        return [e for e in self.entries
                if any(s.volume != e.volume for s in e.structures)]

    def table_spans_volumes(self):
        return len(set(s.volume for s in self.table_structures)) > 1


def _advance(vs, volume, offset):
    """Placement rule: a structure that ended exactly at the end of a volume
    body is followed by offset 32 of the next volume."""
    if offset == vs.body_end(volume):
        return volume + 1, VOLUME_HEADER
    return volume, offset


def parse_set(paths):
    """Parse the volume files of one intact set.  paths is any iterable of
    file paths; volumes are identified by their headers, not their names."""
    vs = VolumeSet()
    for path in paths:
        with open(path, "rb") as f:
            raw = f.read()
        if raw[:4] != b"BOLT":
            raise ValueError("%s: not a volume" % path)
        number = struct.unpack_from("<H", raw, 16)[0]
        set_id = struct.unpack_from("<Q", raw, 8)[0]
        if vs.set_id and vs.set_id != set_id:
            raise ValueError("%s: different set id" % path)
        vs.set_id = set_id
        vs.paths[number] = path
        vs.data[number] = raw
    count = max(vs.paths) + 1
    if sorted(vs.paths) != list(range(count)):
        raise ValueError("set is not complete: %s" % sorted(vs.paths))
    vs.volume_count = count

    for v in range(count):
        raw = vs.data[v]
        vs.structures.append(Structure(KIND_VOLUME_HEADER, v, 0,
                                       VOLUME_HEADER))
        vs.structures.append(Structure(KIND_VOLUME_TRAILER, v,
                                       len(raw) - VOLUME_TRAILER,
                                       VOLUME_TRAILER))
        size = struct.unpack_from("<Q", raw, len(raw) - 16)[0]
        if size != len(raw):
            raise ValueError("volume %d size field %d != %d" %
                             (v, size, len(raw)))

    last = vs.data[count - 1]
    end_at = len(last) - VOLUME_TRAILER - END_RECORD
    (set_id, vcount, tvol, tcrc, toff, tsize, ecount, _res, ecrc,
     emagic) = struct.unpack_from("<QHHIQQIIII", last, end_at)
    if emagic != 0x424F4C54 or ecrc != crc32(last[end_at:end_at + 40]):
        raise ValueError("bad end record")
    if vcount != count or set_id != vs.set_id:
        raise ValueError("end record disagrees with the volumes")
    vs.table_volume, vs.table_offset, vs.table_size, vs.table_crc = (
        tvol, toff, tsize, tcrc)
    vs.end_record = Structure(KIND_END_RECORD, count - 1, end_at, END_RECORD)
    vs.structures.append(vs.end_record)

    # The table: concatenation of its structures across volumes.
    table = bytearray()
    vol, off = tvol, toff
    left = tsize
    while left > 0:
        take = min(left, vs.body_end(vol) - off)
        table += vs.data[vol][off:off + take]
        vs.table_structures.append(Structure(KIND_TABLE, vol, off, take))
        left -= take
        vol, off = vol + 1, VOLUME_HEADER
    if crc32(table) != tcrc:
        raise ValueError("table CRC mismatch")
    vs.structures.extend(vs.table_structures)
    magic, tcount, _total = struct.unpack_from("<4sIQ", table, 0)
    if magic != b"TBLT" or tcount != ecount:
        raise ValueError("bad table header")

    pos = 16
    for i in range(tcount):
        e = Entry()
        e.index = i
        nlen = struct.unpack_from("<H", table, pos)[0]
        e.name = bytes(table[pos + 2:pos + 2 + nlen])
        f = pos + 2 + nlen
        (e.original_size, e.compressed_size, e.entry_type, e.platform,
         e.mtime, e.volume, e.offset, e.method, e.header_size, e.checksum,
         e.flags) = struct.unpack_from("<QQBBQHQBHIH", table, f)
        pos = f + 45
        vs.entries.append(e)

    for e in vs.entries:
        _walk_entry(vs, e)
    return vs


def _walk_entry(vs, e):
    raw = vs.data[e.volume]
    off = e.offset
    hs, nl = struct.unpack_from("<HH", raw, off)
    if hs != ENTRY_FIXED + nl or hs != e.header_size:
        raise ValueError("entry %d: header size" % e.index)
    if crc32(raw[off:off + hs - 4]) != struct.unpack_from("<I", raw,
                                                         off + hs - 4)[0]:
        raise ValueError("entry %d: header CRC" % e.index)
    inline = off + 4 + nl
    e.block_size = struct.unpack_from("<I", raw, inline + 10)[0]
    st = Structure(KIND_ENTRY_HEADER, e.volume, off, hs, e.index)
    e.structures.append(st)
    vs.structures.append(st)
    vol, pos = _advance(vs, e.volume, off + hs)
    left = e.original_size
    comp_total = 0
    i = 0
    while left > 0:
        raw = vs.data[vol]
        comp, orig, bcrc = struct.unpack_from("<III", raw, pos)
        if crc32(raw[pos + 12:pos + 12 + comp]) != bcrc:
            raise ValueError("entry %d block %d CRC" % (e.index, i))
        st = Structure(KIND_BLOCK, vol, pos, 12 + comp, e.index, i)
        e.structures.append(st)
        vs.structures.append(st)
        e.blocks.append((orig, comp))
        comp_total += 12 + comp
        left -= orig
        i += 1
        vol, pos = _advance(vs, vol, pos + 12 + comp)
    if left != 0:
        raise ValueError("entry %d: block sizes overshoot" % e.index)
    raw = vs.data[vol]
    csize, csum, tcrc = struct.unpack_from("<QII", raw, pos)
    if tcrc != crc32(raw[pos:pos + 12]) or csize != comp_total:
        raise ValueError("entry %d: trailer" % e.index)
    if comp_total != e.compressed_size:
        raise ValueError("entry %d: compressed size" % e.index)
    st = Structure(KIND_ENTRY_TRAILER, vol, pos, 16, e.index)
    e.structures.append(st)
    vs.structures.append(st)
    e.end_volume = vol
    e.end_offset = pos + 16


def check_chain(vs):
    """Verify the archive stream is gap-free: each entry starts where the last
    ended, and the table starts where the last entry ended.  Raises on any
    discrepancy; used to make sure the oracle itself is right."""
    vol, pos = 0, VOLUME_HEADER
    for e in vs.entries:
        if (e.volume, e.offset) != (vol, pos):
            raise ValueError("entry %d at (%d,%d), chain says (%d,%d)" %
                             (e.index, e.volume, e.offset, vol, pos))
        vol, pos = _advance(vs, e.end_volume, e.end_offset)
    if (vs.table_volume, vs.table_offset) != (vol, pos):
        raise ValueError("table at (%d,%d), chain says (%d,%d)" %
                         (vs.table_volume, vs.table_offset, vol, pos))


def raw_header_hits(data, lo=VOLUME_HEADER, hi=None):
    """Offsets in data where an entry header would pass the header checks:
    header_size == 34 + name_len and a matching CRC-32.  A deliberately
    simple Python scan, used by fixture checks only (never as an expected
    result)."""
    hi = len(data) - 4 if hi is None else hi
    out = []
    for i in range(lo, min(hi, len(data) - ENTRY_FIXED)):
        hs, nl = struct.unpack_from("<HH", data, i)
        if hs != ENTRY_FIXED + nl or nl == 0 or i + hs > len(data):
            continue
        if crc32(data[i:i + hs - 4]) == struct.unpack_from("<I", data,
                                                           i + hs - 4)[0]:
            out.append(i)
    return out
