#!/usr/bin/env python3
"""Generate reproducible full-block and format-boundary BoltLZ fuzz seeds.

Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
"""
from __future__ import annotations

import argparse
import hashlib
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def var(value: int) -> bytes:
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/boltlz/fuzz-validation-corpus")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261014)
    count = 0

    def add(data: bytes) -> None:
        nonlocal count
        assert len(data) <= 262144
        (args.output / hashlib.sha256(data).hexdigest()).write_bytes(data)
        count += 1

    for size in (0, 1, 3, 4, 15, 16, 17, 32768, 65535, 65536, 65537, 131072, 262144):
        add(bytes(size))
        pattern = rng.randbytes(1024)
        add((pattern * ((size + 1023) // 1024))[:size])
    add(rng.randbytes(262144))
    # Three leading bytes tell the fuzz harness the full expected output size.
    # Explicit long distances reach the 16/32-bit table transition and the
    # maximum block, with independent valid and malformed stream seeds.
    for distance in (1, 3, 7, 15, 16, 127, 128, 16383, 16384, 65535, 65536, 131071, 262000):
        literals = (b"abcdefg" * ((distance + 6) // 7))[:distance]
        literal_code = min(distance, 7)
        match = 124
        stream = (b"\x02" + bytes([(literal_code << 4) | 15]) +
                  (var(distance - 7) if literal_code == 7 else b"") +
                  literals + var(match - 19) + var(distance))
        expected = distance + match
        prefix = expected.to_bytes(3, "big")
        add(prefix + stream)
        add(prefix + stream[:-1])
        add(prefix + stream[:-1] + b"\x80")
        if expected + 19 <= 262144:
            # Repeat the previous distance using a match-length extension.
            add((expected + 19).to_bytes(3, "big") + stream + b"\x8f\x00")
    print(f"Wrote {count} seed inputs; existing evolved seeds are preserved")


if __name__ == "__main__":
    main()
