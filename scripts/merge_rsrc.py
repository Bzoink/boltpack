#!/usr/bin/env python3
"""
merge_rsrc.py -- Merge two Mac OS Classic HFS resource forks.

Usage:
    python3 merge_rsrc.py base.rsrc overlay.rsrc output.rsrc

Resources from base are kept; resources from overlay are added on top.
When a (type, id) exists in both files, overlay wins.

This is used in the BoltPack build to overlay the GUI resources compiled
by Rez on top of the CODE/DATA resources produced by the Retro68 linker.
"""

import struct
import sys


def parse(data):
    """Parse a binary resource fork.

    Returns a list of (rtype, res_id, name, attrs, payload) tuples.
    rtype is a 4-char str decoded as mac_roman.
    name is a str (empty string if absent).
    attrs is an int (resource attributes byte).
    payload is bytes.
    """
    if len(data) < 16:
        return []

    # Rez writes a 256-byte preamble of zeros before the real header.
    # Detect this: a valid resource fork has data_off >= 16 and
    # data_off < len(data). If the first 4 bytes look like a pointer
    # into a 256-byte-offset file, skip the preamble.
    offset = 0
    data_off_check = struct.unpack_from(">I", data, 0)[0]
    if data_off_check == 0 and len(data) > 256:
        # Likely a 256-byte preamble; real header starts at offset 256.
        candidate = struct.unpack_from(">I", data, 256)[0]
        if candidate >= 16:
            offset = 256
    data = data[offset:]

    if len(data) < 16:
        return []

    data_off = struct.unpack_from(">I", data, 0)[0]
    map_off = struct.unpack_from(">I", data, 4)[0]

    if map_off + 30 > len(data):
        return []

    type_list_off = map_off + 28
    n_types = struct.unpack_from(">H", data, type_list_off)[0] + 1
    type_list_start = type_list_off + 2

    name_list_rel = struct.unpack_from(">H", data, map_off + 26)[0]
    name_list_off = map_off + name_list_rel

    resources = []
    for i in range(n_types):
        t = type_list_start + i * 8
        rtype = data[t : t + 4].decode("mac_roman")
        count = struct.unpack_from(">H", data, t + 4)[0] + 1
        ref_off_rel = struct.unpack_from(">H", data, t + 6)[0]
        ref_off = type_list_off + ref_off_rel

        for j in range(count):
            r = ref_off + j * 12
            res_id = struct.unpack_from(">h", data, r)[0]
            name_off_rel = struct.unpack_from(">H", data, r + 2)[0]
            attrs = data[r + 4]
            doff_rel = (data[r + 5] << 16) | (data[r + 6] << 8) | data[r + 7]
            abs_d = data_off + doff_rel
            res_size = struct.unpack_from(">I", data, abs_d)[0]
            payload = bytes(data[abs_d + 4 : abs_d + 4 + res_size])

            name = ""
            if name_off_rel != 0xFFFF:
                n = name_list_off + name_off_rel
                nlen = data[n]
                name = data[n + 1 : n + 1 + nlen].decode("mac_roman", errors="replace")

            resources.append((rtype, res_id, name, attrs, payload))

    return resources


def build(resources):
    """Serialise a list of (rtype, res_id, name, attrs, payload) into a
    binary resource fork.
    """
    # ---- data section ------------------------------------------------
    data_parts = []
    data_offsets = []
    pos = 0
    for (_rtype, _res_id, _name, _attrs, payload) in resources:
        data_offsets.append(pos)
        data_parts.append(struct.pack(">I", len(payload)) + payload)
        pos += 4 + len(payload)
    data_section = b"".join(data_parts)

    # ---- name list ---------------------------------------------------
    name_offsets = {}   # index in resources -> offset in name list
    name_parts = []
    name_pos = 0
    for idx, (_rtype, _res_id, name, _attrs, _payload) in enumerate(resources):
        if name:
            name_offsets[idx] = name_pos
            encoded = name.encode("mac_roman", errors="replace")[:255]
            name_parts.append(bytes([len(encoded)]) + encoded)
            name_pos += 1 + len(encoded)
    name_section = b"".join(name_parts)

    # ---- group resources by type (sorted for determinism) ------------
    type_order = sorted({r[0] for r in resources})
    type_groups = {rt: [] for rt in type_order}
    for idx, r in enumerate(resources):
        type_groups[r[0]].append(idx)

    # ---- compute ref-list offsets (relative to type_list_off=map+28) -
    n_types = len(type_order)
    type_list_size = 2 + n_types * 8   # count word + n * 8-byte entry
    ref_list_offsets = {}
    ref_pos = type_list_size
    for rt in type_order:
        ref_list_offsets[rt] = ref_pos
        ref_pos += len(type_groups[rt]) * 12

    name_list_off_in_map = 28 + ref_pos   # relative to map start

    # ---- build map section -------------------------------------------
    map_buf = bytearray()
    map_buf += b"\x00" * 16    # copy of file header (filled below)
    map_buf += b"\x00" * 4     # next map handle
    map_buf += b"\x00" * 2     # file reference
    map_buf += b"\x00" * 2     # resource file attributes
    map_buf += struct.pack(">H", 28)                      # type-list offset in map
    map_buf += struct.pack(">H", name_list_off_in_map)    # name-list offset in map

    # type list: count-1, then entries
    map_buf += struct.pack(">H", n_types - 1)
    for rt in type_order:
        indices = type_groups[rt]
        map_buf += rt.encode("mac_roman")
        map_buf += struct.pack(">H", len(indices) - 1)
        map_buf += struct.pack(">H", ref_list_offsets[rt])

    # ref lists
    for rt in type_order:
        for idx in type_groups[rt]:
            _rt, res_id, _name, attrs, _payload = resources[idx]
            no = name_offsets.get(idx, 0xFFFF)
            doff = data_offsets[idx]
            map_buf += struct.pack(">h", res_id)
            map_buf += struct.pack(">H", no)
            map_buf += bytes([attrs])
            map_buf += bytes([(doff >> 16) & 0xFF, (doff >> 8) & 0xFF, doff & 0xFF])
            map_buf += b"\x00" * 4   # placeholder (filled by Resource Manager at load)

    # name list
    map_buf += name_section

    map_section = bytes(map_buf)

    # ---- assemble the fork -------------------------------------------
    header_size = 256
    data_off = header_size
    map_off = data_off + len(data_section)

    # Main header (16 bytes) followed by 240 bytes of reserved space
    header = struct.pack(">IIII", data_off, map_off, len(data_section), len(map_section))
    header += b"\x00" * (256 - len(header))

    # Fix the copy of the header at the start of the map section
    map_section = (
        struct.pack(">IIII", data_off, map_off, len(data_section), len(map_section))
        + map_section[16:]
    )

    return header + data_section + map_section


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} base.rsrc overlay.rsrc output.rsrc")
        sys.exit(1)

    base_path, overlay_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(base_path, "rb") as f:
        base_data = f.read()
    with open(overlay_path, "rb") as f:
        overlay_data = f.read()

    base_res = parse(base_data)
    overlay_res = parse(overlay_data)

    # Merge: base first, then overlay; overlay wins on type+id collision.
    merged = {}
    for r in base_res:
        merged[(r[0], r[1])] = r
    for r in overlay_res:
        merged[(r[0], r[1])] = r

    # Sort by type then id for deterministic output
    combined = sorted(merged.values(), key=lambda r: (r[0], r[1]))

    result = build(combined)

    with open(out_path, "wb") as f:
        f.write(result)

    n_base = len(base_res)
    n_over = len(overlay_res)
    n_out = len(combined)
    print(
        f"merge_rsrc: {n_base} base + {n_over} overlay = {n_out} resources "
        f"-> {out_path} ({len(result)} bytes)"
    )


if __name__ == "__main__":
    main()
