#!/usr/bin/env python3
"""Prepare the pinned first-evaluation BoltLZ corpora without tuning the codec.

Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "test/reference/boltlz-holdout.json"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def check_files(directory: Path, expected: list[dict]) -> None:
    paths = list(directory.rglob("*"))
    if any(p.is_symlink() or not p.is_file() for p in paths):
        raise ValueError(f"Unexpected directory or symlink in {directory}")
    if {p.name for p in paths} != {f["name"] for f in expected}:
        raise ValueError(f"File selection differs from pinned manifest: {directory}")
    for item in expected:
        data = (directory / item["name"]).read_bytes()
        if len(data) != item["size"] or digest(data) != item["sha256"]:
            raise ValueError(f"Corpus file differs: {directory / item['name']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archives", type=Path, default=ROOT / "test/corpus/boltlz-downloads")
    parser.add_argument("--output", type=Path, default=ROOT / "test/corpus/boltlz-holdout-2026-10-14")
    parser.add_argument("--download", action="store_true", help="download missing official archives")
    parser.add_argument("--check-only", action="store_true", help="verify prepared files without downloading/extracting")
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    for corpus in manifest["corpora"]:
        output = args.output / corpus["name"]
        if not args.check_only:
            archive = args.archives / corpus["archive"]
            if not archive.exists() and args.download:
                args.archives.mkdir(parents=True, exist_ok=True)
                with urllib.request.urlopen(corpus["url"], timeout=60) as response:
                    data = response.read(16 * 1024 * 1024 + 1)
                if digest(data) != corpus["archive_sha256"]:
                    raise ValueError(f"Downloaded archive checksum differs: {corpus['name']}")
                archive.write_bytes(data)
            if digest(archive.read_bytes()) != corpus["archive_sha256"]:
                raise ValueError(f"Archive checksum differs: {archive}")
            expected = {f["name"]: f for f in corpus["files"]}
            excluded = set(corpus.get("excluded_archive_members", []))
            with tarfile.open(archive) as tar:
                members = tar.getmembers()
                if (len({m.name for m in members}) != len(members) or
                    {m.name for m in members} != set(expected) | excluded or
                    any(not m.isfile() for m in members)):
                    raise ValueError(f"Archive selection differs: {archive}")
                # Only manifest-listed flat filenames can reach the output.
                for name in expected:
                    if Path(name).name != name or name in (".", ".."):
                        raise ValueError(f"Unsafe member name: {name}")
                if output.exists():
                    check_files(output, corpus["files"])
                else:
                    payloads = {}
                    for name, item in expected.items():
                        member = tar.getmember(name)
                        if member.size != item["size"]:
                            raise ValueError(f"Archive member size differs: {name}")
                        data = tar.extractfile(member).read()
                        if digest(data) != item["sha256"]:
                            raise ValueError(f"Archive member checksum differs: {name}")
                        payloads[name] = data
                    output.mkdir(parents=True)
                    for name, data in payloads.items():
                        (output / name).write_bytes(data)
        check_files(output, corpus["files"])
        print(f"{corpus['name']}: {len(corpus['files'])} pinned files verified")


if __name__ == "__main__":
    main()
