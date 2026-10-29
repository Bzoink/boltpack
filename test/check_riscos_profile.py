#!/usr/bin/env python3
"""Cross-check RISC OS writer output through Linux full and mini readers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "test/reference/interoperability/riscos-host.json"
EXPECTED = json.loads(MANIFEST.read_text(encoding="utf-8"))


def run(command, cwd, expected=(0,)):
    result = subprocess.run([str(item) for item in command], cwd=cwd,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    if result.returncode not in expected:
        raise RuntimeError("command returned %d: %r\n%s" %
                           (result.returncode, command, result.stdout))
    return result


def verify_tree(path):
    for relative, metadata in EXPECTED["files"].items():
        target = path / relative
        if metadata["type"] == "directory":
            if not target.is_dir():
                raise RuntimeError("missing extracted directory: " + relative)
            continue
        data = target.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if len(data) != metadata["bytes"] or digest != metadata["sha256"]:
            raise RuntimeError("payload mismatch for %s: %d bytes, %s" %
                               (relative, len(data), digest))


def primary_mtimes(path):
    archive = path.read_bytes()
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    count = struct.unpack_from("<I", archive, table_offset + 4)[0]
    record = table_offset + 16
    entries = {}
    for _ in range(count):
        name_length = struct.unpack_from("<H", archive, record)[0]
        name = archive[record + 2:record + 2 + name_length].decode("utf-8")
        fields = record + 2 + name_length
        entry_type = archive[fields + 16]
        mtime = struct.unpack_from("<Q", archive, fields + 18)[0]
        flags = struct.unpack_from("<H", archive, fields + 41)[0]
        if entry_type in (0, 3, 4):
            entries[name] = (mtime, flags)
        record = fields + 43
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--riscos-host", required=True, type=Path)
    parser.add_argument("--linux-boltpack", required=True, type=Path)
    parser.add_argument("--mini-boltpack", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="boltpack-riscos-profile-") as tmp:
        root = Path(tmp)
        tree = root / "tree"
        (tree / "nested").mkdir(parents=True)
        (tree / "empty").mkdir()
        (tree / "nested" / "payload").write_bytes(
            (b"RISC OS Profile 1 payload\n" * 19) + bytes(range(32)))
        (tree / "nested" / "readme").write_bytes(b"nested source file\n")
        os.utime(tree / "nested" / "payload", (-1, -1))
        os.utime(tree / "nested", (-1, -1))
        (root / "new").write_bytes(b"appended on RISC OS\n")
        writer = run([args.riscos_host.resolve()], root)
        if "2 files, 3 directories" not in writer.stdout:
            raise RuntimeError("RISC OS create summary counted metadata entries:\n" +
                               writer.stdout)
        archive = root / "archive"
        times = primary_mtimes(archive)
        for suffix in ("nested/payload", "nested/"):
            matches = [value for name, value in times.items()
                       if name.endswith(suffix)]
            if len(matches) != 1 or matches[0][0] != 0 or \
                    matches[0][1] & 0x4000:
                raise RuntimeError("pre-epoch RISC OS time was not encoded unknown: " +
                                   suffix)
        info = run([args.linux_boltpack.resolve(), "i", archive], root).stdout
        if "Format version: 6" not in info or "RISC OS" not in info or "ARM" not in info:
            raise RuntimeError("RISC OS writer identity/profile missing from info:\n" + info)
        if "BoltLite" not in info:
            raise RuntimeError("Profile 1 writer did not select BoltLite:\n" + info)
        readers = ((args.linux_boltpack.resolve(), False),
                   (args.mini_boltpack.resolve(), True))
        for reader, mini in readers:
            run([reader, "t", archive], root)
            output = root / ("out-mini" if mini else "out-full")
            command = ([reader, "x", archive, output] if mini else
                       [reader, "x", "-o", output, archive])
            result = run(command, root, expected=(0, 1))
            if result.returncode != 1 or "attribute" not in result.stdout.lower():
                raise RuntimeError("foreign RISC OS metadata was not reported partial:\n" +
                                   result.stdout)
            verify_tree(output)
        print("RISC OS Profile 1 writer and Linux full/mini transfer checks passed")


if __name__ == "__main__":
    main()
