#!/usr/bin/env python3
"""Target-side smoke tests for the Windows CLI archive edit lifecycle.

Run on Windows with:
    python test/check_windows_cli.py [path-to-boltpack.exe]

This deliberately uses only the Python standard library so it can run in a
clean Windows build environment without pytest or pywin32.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
from pathlib import Path


FILE_ATTRIBUTE_HIDDEN = 0x00000002
DEFAULT_BINARY = Path(__file__).resolve().parents[1] / "build" / "win64" / "boltpack.exe"


def fail(message: str) -> None:
    raise AssertionError(message)


def run(binary: Path, cwd: Path, *args: str, success: bool = True) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run([str(binary), *args], cwd=cwd, capture_output=True)
    if (result.returncode == 0) != success:
        fail(
            f"command {args!r} returned {result.returncode}; "
            f"stdout={result.stdout!r}; stderr={result.stderr!r}"
        )
    return result


def set_hidden(path: Path) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetFileAttributesW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetFileAttributesW.restype = ctypes.c_uint32
    kernel32.SetFileAttributesW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
    kernel32.SetFileAttributesW.restype = ctypes.c_int
    attrs = kernel32.GetFileAttributesW(str(path))
    if attrs == 0xFFFFFFFF:
        raise ctypes.WinError(ctypes.get_last_error())
    if not kernel32.SetFileAttributesW(str(path), attrs | FILE_ATTRIBUTE_HIDDEN):
        raise ctypes.WinError(ctypes.get_last_error())


def is_hidden(path: Path) -> bool:
    return bool(path.stat().st_file_attributes & FILE_ATTRIBUTE_HIDDEN)


def main() -> int:
    if os.name != "nt":
        print("SKIP: this integration check requires Windows")
        return 77

    parser = argparse.ArgumentParser()
    parser.add_argument("binary", nargs="?", type=Path, default=DEFAULT_BINARY)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        fail(f"BoltPack Windows CLI does not exist: {binary}")

    with tempfile.TemporaryDirectory(prefix="boltpack-win-edit-") as temp:
        root = Path(temp)
        source = root / "src"
        source.mkdir()
        (source / "original.txt").write_bytes(b"original archive member\n")
        (source / "added.txt").write_bytes(b"new member after append\n")
        archive = root / "archive.bolt"

        run(binary, root, "c", str(archive), "src/original.txt")
        set_hidden(archive)
        original = archive.read_bytes()

        alias = root / "archive-alias.bolt"
        try:
            os.link(archive, alias)
        except OSError as error:
            print(f"SKIP: hard-link refusal fixture unavailable: {error}")
        else:
            for result in (
                run(binary, root, "a", str(archive), "src/added.txt", success=False),
                run(binary, root, "d", str(archive), "src/original.txt", success=False),
            ):
                if b"single-link" not in result.stderr:
                    fail("hard-link edit refusal did not report its target constraint")
            if archive.read_bytes() != original or alias.read_bytes() != original:
                fail("refused hard-link edits changed archive bytes")
            if not os.path.samefile(archive, alias):
                fail("refused hard-link edits changed the hard-link relationship")
            alias.unlink()
            print("PASS: append/delete refuse multiply linked archive files")

        symlink = root / "archive-link.bolt"
        try:
            os.symlink(archive, symlink)
        except OSError as error:
            print(f"SKIP: reparse-point refusal fixture unavailable: {error}")
        else:
            for result in (
                run(binary, root, "a", str(symlink), "src/added.txt", success=False),
                run(binary, root, "d", str(symlink), "src/original.txt", success=False),
            ):
                if b"single-link" not in result.stderr:
                    fail("reparse-point edit refusal did not report its target constraint")
            if archive.read_bytes() != original:
                fail("refused reparse-point edits changed the target archive")
            symlink.unlink()
            print("PASS: append/delete refuse reparse-point archive targets")

        run(binary, root, "a", str(archive), "src/added.txt")
        run(binary, root, "t", str(archive))
        if not is_hidden(archive):
            fail("append did not preserve FILE_ATTRIBUTE_HIDDEN")
        listing = run(binary, root, "l", str(archive))
        if b"original.txt" not in listing.stdout or b"added.txt" not in listing.stdout:
            fail("append listing is missing an expected entry")
        print("PASS: append publishes a readable archive and retains hidden attribute")

        run(binary, root, "d", str(archive), "src/original.txt")
        run(binary, root, "t", str(archive))
        if not is_hidden(archive):
            fail("delete did not preserve FILE_ATTRIBUTE_HIDDEN")
        listing = run(binary, root, "l", str(archive))
        if b"original.txt" in listing.stdout or b"added.txt" not in listing.stdout:
            fail("delete listing does not match the requested edit")
        print("PASS: delete publishes a readable archive and retains hidden attribute")

    print("Windows archive edit checks passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
