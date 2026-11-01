#!/usr/bin/env python3
"""Verify Classic Mac app identity agrees between source and MacBinary output."""

import argparse
import re
from pathlib import Path


REQUIRED_RESOURCE_TYPES = {
    "ALRT", "CODE", "DATA", "DITL", "DLOG", "MBAR", "MENU", "RELA",
    "SIZE", "WIND", "vers",
}


def macbinary_resource_types(data: bytes) -> set[str]:
    if len(data) < 128 or data[0] != 0 or data[1] == 0:
        raise SystemExit("application output is not a valid MacBinary header")

    data_length = int.from_bytes(data[83:87], "big")
    resource_length = int.from_bytes(data[87:91], "big")
    resource_start = 128 + ((data_length + 127) // 128) * 128
    resource_end = resource_start + resource_length
    if resource_end > len(data) or resource_length < 30:
        raise SystemExit("application output has a missing or truncated resource fork")

    fork = data[resource_start:resource_end]
    map_offset = int.from_bytes(fork[4:8], "big")
    map_length = int.from_bytes(fork[12:16], "big")
    if map_offset + map_length > len(fork) or map_length < 28:
        raise SystemExit("application output has an invalid resource map")

    resource_map = fork[map_offset:map_offset + map_length]
    type_list_offset = int.from_bytes(resource_map[24:26], "big")
    if type_list_offset + 2 > len(resource_map):
        raise SystemExit("application output has an invalid resource type list")
    type_count = int.from_bytes(
        resource_map[type_list_offset:type_list_offset + 2], "big"
    ) + 1
    entries_start = type_list_offset + 2
    entries_end = entries_start + type_count * 8
    if entries_end > len(resource_map):
        raise SystemExit("application output has a truncated resource type list")

    types = set()
    for index in range(type_count):
        entry_start = entries_start + index * 8
        types.add(resource_map[entry_start:entry_start + 4].decode("mac_roman"))
    return types


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--makefile", required=True, type=Path)
    parser.add_argument("--application", required=True, type=Path)
    args = parser.parse_args()

    source = args.source.read_text(encoding="utf-8")
    makefile = args.makefile.read_text(encoding="utf-8")
    source_match = re.search(r"^#define\s+kCreatorCode\s+'([^']{4})'", source,
                             re.MULTILINE)
    package_match = re.search(
        r"BoltPack\.bin\s+-t\s+APPL\s+-c\s+([A-Za-z0-9]{4})",
        makefile,
    )
    if source_match is None or package_match is None:
        raise SystemExit("could not find the app and package creator declarations")

    expected_creator = source_match.group(1).encode("ascii")
    package_creator = package_match.group(1).encode("ascii")
    if package_creator != expected_creator:
        raise SystemExit(
            f"package creator {package_creator!r} differs from app creator "
            f"{expected_creator!r}"
        )

    data = args.application.read_bytes()
    file_type = data[65:69]
    file_creator = data[69:73]
    if file_type != b"APPL" or file_creator != expected_creator:
        raise SystemExit(
            f"MacBinary identity is {file_type!r}/{file_creator!r}; expected "
            f"b'APPL'/{expected_creator!r}"
        )
    resource_types = macbinary_resource_types(data)
    missing_types = REQUIRED_RESOURCE_TYPES - resource_types
    if missing_types:
        raise SystemExit(
            "application resource fork is missing required GUI/code resources: "
            + ", ".join(sorted(missing_types))
        )
    print(
        f"Classic Mac app package verified: APPL/{expected_creator.decode('ascii')}; "
        f"resource types {', '.join(sorted(resource_types))}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
