#!/usr/bin/env python3
"""Inspect a Classic Mac resource fork (.rsrc) file.

This script parses the resource map and prints a summary of resource types,
IDs, names, and data sizes.

Usage:
  ./inspect_rsrc.py <path/to/file.rsrc>

Example:
  ./inspect_rsrc.py ../resources/macos9/RESOURCE.FRK/WINDOW~1.RSR
"""

import os
import struct
import sys


def read_be(fmt, data, offset=0):
    """Read big-endian primitive from bytes."""
    size = struct.calcsize('>' + fmt)
    if offset + size > len(data):
        raise IndexError('read past end of buffer')
    return struct.unpack('>' + fmt, data[offset:offset + size])[0]


def parse_resource_fork(path):
    with open(path, 'rb') as f:
        data = f.read()

    if len(data) < 16:
        raise ValueError('file too small to be a resource fork')

    data_off = read_be('I', data, 0)
    map_off = read_be('I', data, 4)
    data_len = read_be('I', data, 8)
    map_len = read_be('I', data, 12)

    if map_off + map_len > len(data):
        raise ValueError('resource map truncated')

    map_bytes = data[map_off:map_off + map_len]
    if len(map_bytes) < 28:
        raise ValueError('resource map too small')

    # Resource map structure (after the 16-byte file header copy):
    #   2 bytes file attributes
    #   2 bytes offset to type list (from start of map)
    #   2 bytes offset to name list (from start of map)
    # NOTE: In real resource forks these offsets start at byte 24 of the map
    # (after the 16-byte header + 8 bytes of reserved/attributes data).
    type_list_off = read_be('H', map_bytes, 24)
    name_list_off = read_be('H', map_bytes, 26)

    type_list_pos = map_off + type_list_off
    if type_list_pos + 2 > len(data):
        raise ValueError('type list out of range')

    num_types = read_be('H', data, type_list_pos) + 1
    out = []

    pos = type_list_pos + 2
    for _ in range(num_types):
        if pos + 8 > len(data):
            break
        rtype = data[pos:pos + 4].decode('ascii', errors='replace')
        count = read_be('H', data, pos + 4) + 1
        ref_list_off = read_be('H', data, pos + 6)
        # ref_list_off is relative to the start of the type list, not the start of the map
        ref_list_pos = type_list_pos + ref_list_off

        resources = []
        for ri in range(count):
            ent_off = ref_list_pos + ri * 12
            if ent_off + 12 > len(data):
                break
            res_id = read_be('h', data, ent_off)
            name_off = read_be('H', data, ent_off + 2)
            attr = data[ent_off + 4]
            data_off_24 = data[ent_off + 5:ent_off + 8]
            data_off_rel = int.from_bytes(data_off_24, 'big')
            data_pos = data_off + data_off_rel

            name = None
            if name_off != 0xFFFF:
                name_pos = map_off + name_list_off + name_off
                if name_pos < len(data):
                    length = data[name_pos]
                    end = name_pos + 1 + length
                    if end <= len(data):
                        name = data[name_pos + 1:end].decode('mac_roman', errors='replace')

            size = None
            # data is stored as size (4 bytes) + payload
            if data_pos + 4 <= len(data):
                size = read_be('I', data, data_pos)

            resources.append({
                'id': res_id,
                'name': name,
                'attr': attr,
                'data_off': data_pos,
                'data_size': size,
            })

        out.append((rtype, resources))
        pos += 8

    return out


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]

    if len(argv) != 1:
        print(__doc__)
        sys.exit(1)

    path = argv[0]
    if not os.path.isfile(path):
        raise SystemExit(f'File not found: {path}')

    print('Inspecting:', path)

    res = parse_resource_fork(path)
    for rtype, entries in res:
        print(f"\nType {rtype} (count {len(entries)})")
        for e in entries:
            print(f"  id={e['id']:>4} attr=0x{e['attr']:02x} size={e['data_size']} name={e['name']}")


if __name__ == '__main__':
    main()
