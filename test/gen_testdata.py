#!/usr/bin/env python3
"""
gen_testdata.py -- reusable test-data generators for BoltPack tests.

Provides functions to create files of specific sizes, patterns, and names
inside a given directory.  No external dependencies beyond the Python
standard library.

Usage (as a library):
    from gen_testdata import make_file, make_tree, make_sparse_file

Usage (standalone -- generates a sample set in a temp dir):
    python3 test/gen_testdata.py [output_dir]

Copyright (C) 2026 Tony Mattsson
Licensed under the BSD 2-Clause License
"""

import hashlib
import os
import random
import struct
import sys


# ---------------------------------------------------------------------------
# Deterministic pseudo-random generator (xorshift32) so test data is stable
# ---------------------------------------------------------------------------

def _xorshift32(seed: int) -> bytes:
    """Yield pseudo-random bytes using xorshift32."""
    s = seed & 0xFFFFFFFF
    buf = []
    while True:
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= (s >> 17) & 0xFFFFFFFF
        s ^= (s << 5)  & 0xFFFFFFFF
        buf.append(s & 0xFF)
        if len(buf) == 4096:
            yield bytes(buf)
            buf = []


def _rand_bytes(seed: int, size: int) -> bytes:
    """Return exactly *size* pseudo-random bytes."""
    out = bytearray()
    gen = _xorshift32(seed)
    while len(out) < size:
        out.extend(next(gen))
    return bytes(out[:size])


# ---------------------------------------------------------------------------
# Single file makers
# ---------------------------------------------------------------------------

def make_file(path: str, size: int, pattern: str = "random",
              seed: int = 0x1234, content: bytes | None = None) -> bytes:
    """
    Create a file at *path* (including parent directories) and return its
    SHA-256 digest (hex string as bytes object).

    pattern: one of
      "random"   -- deterministic pseudo-random (default)
      "zeros"    -- all 0x00 bytes
      "ones"     -- all 0xFF bytes
      "seq"      -- bytes 0x00..0xFF cycling
      "text"     -- printable ASCII Lorem Ipsum cycling
      "literal"  -- use *content* argument verbatim
    """
    os.makedirs(os.path.dirname(path) if os.path.dirname(path) else ".",
                exist_ok=True)

    if pattern == "literal":
        data = content if content is not None else b""
    elif pattern == "zeros":
        data = b"\x00" * size
    elif pattern == "ones":
        data = b"\xFF" * size
    elif pattern == "seq":
        data = bytes(i % 256 for i in range(size))
    elif pattern == "text":
        chunk = (
            b"The quick brown fox jumps over the lazy dog. "
            b"Pack my box with five dozen liquor jugs. "
        )
        data = (chunk * (size // len(chunk) + 1))[:size]
    else:  # random
        data = _rand_bytes(seed, size)

    with open(path, "wb") as fh:
        fh.write(data)

    return hashlib.sha256(data).hexdigest()


def make_sparse_file(path: str, size: int) -> None:
    """
    Create a sparse file of exactly *size* bytes using a seek past EOF.
    The file appears to be *size* bytes but uses no actual disk blocks for
    the hole region on Linux ext4/xfs.  Used to test 64-bit offset handling
    without allocating gigabytes of actual storage.
    """
    os.makedirs(os.path.dirname(path) if os.path.dirname(path) else ".",
                exist_ok=True)
    with open(path, "wb") as fh:
        if size > 0:
            fh.seek(size - 1)
            fh.write(b"\x00")


def make_symlink(path: str, target: str) -> None:
    """Create a symbolic link *path* -> *target*."""
    os.makedirs(os.path.dirname(path) if os.path.dirname(path) else ".",
                exist_ok=True)
    if os.path.lexists(path):
        os.unlink(path)
    os.symlink(target, path)


# ---------------------------------------------------------------------------
# Multi-file tree builders
# ---------------------------------------------------------------------------

#: Five canonical test files used across multiple archive types
BASIC_FILES: list[dict] = [
    {"name": "text.txt",   "size": 1024,   "pattern": "text",   "seed": 1},
    {"name": "zeros.bin",  "size": 4096,   "pattern": "zeros",  "seed": 2},
    {"name": "random.bin", "size": 8192,   "pattern": "random", "seed": 3},
    {"name": "ones.bin",   "size": 2048,   "pattern": "ones",   "seed": 4},
    {"name": "seq.bin",    "size": 3072,   "pattern": "seq",    "seed": 5},
]

#: Five DOS 8.3-safe test files (uppercase, max 8.3 chars)
DOS_FILES: list[dict] = [
    {"name": "TEXT.TXT",   "size": 512,    "pattern": "text",   "seed": 10},
    {"name": "ZEROS.BIN",  "size": 1024,   "pattern": "zeros",  "seed": 11},
    {"name": "RAND.BIN",   "size": 2048,   "pattern": "random", "seed": 12},
    {"name": "ONES.BIN",   "size": 512,    "pattern": "ones",   "seed": 13},
    {"name": "SEQ.BIN",    "size": 1536,   "pattern": "seq",    "seed": 14},
]

#: UTF-8 filenames with non-ASCII characters
UTF8_FILES: list[dict] = [
    {"name": "arabic_\u0645\u0631\u062d\u0628\u0627.txt",   "size": 256,  "pattern": "text",   "seed": 20},
    {"name": "cjk_\u4e2d\u6587.txt",                         "size": 512,  "pattern": "text",   "seed": 21},
    {"name": "cyrillic_\u043f\u0440\u0438\u0432\u0435\u0442.txt",          "size": 256,  "pattern": "text",   "seed": 22},
    {"name": "greek_\u03b3\u03b5\u03b9\u03b1.txt",           "size": 256,  "pattern": "text",   "seed": 23},
    {"name": "emoji_\U0001f600\U0001f4e6.txt",                "size": 128,  "pattern": "text",   "seed": 24},
]


def make_basic_tree(base_dir: str) -> dict[str, str]:
    """
    Create BASIC_FILES under *base_dir*.
    Returns {filename: sha256_hex} manifest.
    """
    manifest = {}
    for f in BASIC_FILES:
        path = os.path.join(base_dir, f["name"])
        digest = make_file(path, f["size"], f["pattern"], f["seed"])
        manifest[f["name"]] = digest
    return manifest


def make_dos_tree(base_dir: str) -> dict[str, str]:
    """Create DOS 8.3-safe test files under *base_dir*."""
    manifest = {}
    for f in DOS_FILES:
        path = os.path.join(base_dir, f["name"])
        digest = make_file(path, f["size"], f["pattern"], f["seed"])
        manifest[f["name"]] = digest
    return manifest


def make_utf8_tree(base_dir: str) -> dict[str, str]:
    """Create files with non-ASCII UTF-8 names under *base_dir*."""
    manifest = {}
    for f in UTF8_FILES:
        path = os.path.join(base_dir, f["name"])
        digest = make_file(path, f["size"], f["pattern"], f["seed"])
        manifest[f["name"]] = digest
    return manifest


def make_deep_tree(base_dir: str, depth: int = 20,
                   file_size: int = 64) -> dict[str, str]:
    """
    Create a single file at depth levels of nesting.
    Returns {archive_path: sha256_hex} manifest.
    """
    manifest = {}
    segments = [f"level{i:02d}" for i in range(depth)]
    rel_path = os.path.join(*segments, "leaf.txt")
    abs_path = os.path.join(base_dir, rel_path)
    digest = make_file(abs_path, file_size, "text", 30)
    manifest[rel_path.replace(os.sep, "/")] = digest
    return manifest


def make_wide_dir(base_dir: str, count: int = 500,
                  file_size: int = 128) -> dict[str, str]:
    """Create *count* files in a single directory."""
    manifest = {}
    for i in range(count):
        name = f"file_{i:04d}.bin"
        path = os.path.join(base_dir, name)
        digest = make_file(path, file_size, "random", seed=0x5000 + i)
        manifest[name] = digest
    return manifest


# ---------------------------------------------------------------------------
# Standalone entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/bp_testdata"
    print(f"Generating test data under: {out}")
    os.makedirs(out, exist_ok=True)

    print("  basic files ...")
    make_basic_tree(os.path.join(out, "basic"))

    print("  DOS 8.3 files ...")
    make_dos_tree(os.path.join(out, "dos"))

    print("  UTF-8 named files ...")
    make_utf8_tree(os.path.join(out, "utf8"))

    print("  deep tree (20 levels) ...")
    make_deep_tree(os.path.join(out, "deep"))

    print("  wide dir (500 files) ...")
    make_wide_dir(os.path.join(out, "wide"))

    print("  sparse file (4 GB + 1) ...")
    make_sparse_file(os.path.join(out, "sparse", "large.bin"),
                     4 * 1024 * 1024 * 1024 + 1)

    print("Done.")
