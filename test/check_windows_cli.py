#!/usr/bin/env python3
"""Target-side smoke tests for the Windows CLI archive edit lifecycle.

Run on Windows with:
    python test/check_windows_cli.py [path-to-boltpack.exe]

For early 64-bit runtime feedback through Wine, pass ``--wine`` and use a
64-bit Wine prefix. Wine results skip reparse-point checks when Wine does not
expose a host symlink as a Windows reparse point; attribute merging is checked
and reported as Wine evidence rather than native Windows evidence.

This deliberately uses only the Python standard library so it can run in a
clean Windows build environment without pytest or pywin32.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


FILE_ATTRIBUTE_HIDDEN = 0x00000002
FILE_ATTRIBUTE_SYSTEM = 0x00000004
DEFAULT_BINARY = Path(__file__).resolve().parents[1] / "build" / "win64" / "boltpack.exe"
WINE_RUNNER: str | None = None
EXPECTED = json.loads((Path(__file__).resolve().parent / "reference" /
                       "interoperability" / "windows-cli.json").read_text(encoding="utf-8"))
ATTRIBUTE_MASKS = {
    "R": 0x00000001,
    "H": FILE_ATTRIBUTE_HIDDEN,
    "S": FILE_ATTRIBUTE_SYSTEM,
}


def check_manifest_file(path: Path, key: str) -> None:
    expected = EXPECTED["files"][key]
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != expected["bytes"] or digest != expected["sha256"]:
        fail(f"independent expected-result mismatch for {key}: "
             f"{len(data)} bytes, sha256={digest}")


def wine_environment() -> dict[str, str]:
    return os.environ.copy()


def wine_path(path: str | Path) -> str:
    try:
        result = subprocess.run(
            ["winepath", "-w", str(path)], capture_output=True, text=True,
            env=wine_environment(), check=True, timeout=20,
        )
    except subprocess.TimeoutExpired as error:
        fail(f"winepath timed out converting {path!s}: {error}")
    return result.stdout.strip()


def fail(message: str) -> None:
    raise AssertionError(message)


@contextmanager
def temporary_directory_cleanup_on_success(prefix: str):
    """Keep failed fixtures for diagnosis; remove the tree after a pass."""
    path = Path(tempfile.mkdtemp(prefix=prefix))
    try:
        yield path
    except BaseException:
        raise
    else:
        shutil.rmtree(path)


def run(binary: Path, cwd: Path, *args: str, success: bool = True) -> subprocess.CompletedProcess[bytes]:
    command = [str(binary), *args]
    if WINE_RUNNER is not None:
        command = [WINE_RUNNER, str(binary)] + [
            wine_path(value) if os.path.isabs(value) else value for value in args
        ]
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True,
                                env=wine_environment() if WINE_RUNNER else None,
                                timeout=90)
    except subprocess.TimeoutExpired as error:
        fail(f"command {args!r} timed out after 90 seconds: {error}")
    if (result.returncode == 0) != success:
        fail(
            f"command {args!r} returned {result.returncode}; "
            f"stdout={result.stdout!r}; stderr={result.stderr!r}"
        )
    return result


def set_attributes(path: Path, *attributes: str) -> None:
    if WINE_RUNNER is not None:
        path_arg = wine_path(path)
        for name in attributes:
            subprocess.run([WINE_RUNNER, "cmd", "/c", "attrib",
                            "+" + name.lower(), path_arg], check=True,
                           env=wine_environment(), capture_output=True)
        return
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetFileAttributesW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetFileAttributesW.restype = ctypes.c_uint32
    kernel32.SetFileAttributesW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
    kernel32.SetFileAttributesW.restype = ctypes.c_int
    attrs = kernel32.GetFileAttributesW(str(path))
    if attrs == 0xFFFFFFFF:
        raise ctypes.WinError(ctypes.get_last_error())
    desired = attrs
    for name in attributes:
        desired |= ATTRIBUTE_MASKS[name]
    if not kernel32.SetFileAttributesW(str(path), desired):
        raise ctypes.WinError(ctypes.get_last_error())


def remove_attributes(path: Path, *attributes: str) -> None:
    if WINE_RUNNER is not None:
        path_arg = wine_path(path)
        for name in attributes:
            subprocess.run([WINE_RUNNER, "cmd", "/c", "attrib",
                            "-" + name.lower(), path_arg], check=True,
                           env=wine_environment(), capture_output=True)
        return
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetFileAttributesW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetFileAttributesW.restype = ctypes.c_uint32
    kernel32.SetFileAttributesW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
    kernel32.SetFileAttributesW.restype = ctypes.c_int
    attrs = kernel32.GetFileAttributesW(str(path))
    if attrs == 0xFFFFFFFF:
        raise ctypes.WinError(ctypes.get_last_error())
    desired = attrs
    for name in attributes:
        desired &= ~ATTRIBUTE_MASKS[name]
    if desired == 0:
        desired = 0x00000080  # FILE_ATTRIBUTE_NORMAL
    if not kernel32.SetFileAttributesW(str(path), desired):
        raise ctypes.WinError(ctypes.get_last_error())


def has_attributes(path: Path, *attributes: str) -> bool:
    if WINE_RUNNER is not None:
        result = subprocess.run([WINE_RUNNER, "cmd", "/c", "attrib", wine_path(path)],
                                check=True, env=wine_environment(),
                                capture_output=True, text=True)
        fields = result.stdout.splitlines()[0].split()[:-1]
        flags = "".join(fields)
        return all(name in flags for name in attributes)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetFileAttributesW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetFileAttributesW.restype = ctypes.c_uint32
    current = kernel32.GetFileAttributesW(str(path))
    if current == 0xFFFFFFFF:
        raise ctypes.WinError(ctypes.get_last_error())
    return all(current & ATTRIBUTE_MASKS[name] for name in attributes)


def is_hidden(path: Path) -> bool:
    return has_attributes(path, "H")


def check_archive_attributes_after_edit(path: Path, operation: str) -> None:
    if not has_attributes(path, "H", "S"):
        fail(f"{operation} did not preserve FILE_ATTRIBUTE_HIDDEN/SYSTEM")
    elif WINE_RUNNER is not None:
        print(f"PASS: Wine fixture confirms archive HIDDEN/SYSTEM after {operation}")
    else:
        print(f"PASS: {operation} preserves archive HIDDEN/SYSTEM")


def primary_entry_properties(path: Path) -> dict[str, tuple[int, int]]:
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
        entry_offset = struct.unpack_from("<Q", archive, fields + 26)[0]
        inline = entry_offset + 4 + name_length
        method = archive[inline + 20]
        block_size = struct.unpack_from("<I", archive, inline + 22)[0]
        if entry_type in (0, 3, 4):
            entries[name] = (method, block_size)
        record = fields + 43
    return entries


def primary_entry_times(path: Path) -> dict[str, tuple[int, int]]:
    archive = path.read_bytes()
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    count = struct.unpack_from("<I", archive, table_offset + 4)[0]
    record = table_offset + 16
    entries = {}
    for _ in range(count):
        name_length = struct.unpack_from("<H", archive, record)[0]
        name = archive[record + 2:record + 2 + name_length].decode("utf-8")
        fields = record + 2 + name_length
        mtime = struct.unpack_from("<Q", archive, fields + 18)[0]
        flags = struct.unpack_from("<H", archive, fields + 41)[0]
        entries[name] = (mtime, flags)
        record = fields + 43
    return entries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", nargs="?", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--wine", help="run the 64-bit Windows binary through Wine")
    parser.add_argument("--linux-cli", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/linux/boltpack",
                        help="Linux CLI for cross-platform checks in Wine mode")
    parser.add_argument("--linux-mini", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/linux32mini/bpmini",
                        help="Linux mini reader for Wine profile checks")
    parser.add_argument("--mini-runner", default="qemu-i386",
                        help="runner for the 32-bit Linux mini binary")
    args = parser.parse_args()
    global WINE_RUNNER
    WINE_RUNNER = args.wine
    if os.name != "nt" and WINE_RUNNER is None:
        print("SKIP: this integration check requires Windows or --wine")
        return 77
    binary = args.binary.resolve()
    if not binary.is_file():
        fail(f"BoltPack Windows CLI does not exist: {binary}")

    with temporary_directory_cleanup_on_success(
            prefix="boltpack-win-edit-") as root:
        unicode_dir = root / "資料-🧪"
        unicode_dir.mkdir()
        unicode_member = unicode_dir / "résumé-🧪.txt"
        unicode_bytes = b"UTF-16 Win32 path interoperability\n"
        unicode_member.write_bytes(unicode_bytes)
        unicode_archive = root / "archive-🧪.bolt"
        run(binary, root, "c", str(unicode_archive),
            unicode_member.relative_to(root).as_posix())
        unicode_listing = run(binary, root, "l", str(unicode_archive))
        check_manifest_file(unicode_member, "unicode")
        expected_name = EXPECTED["names"]["unicode_path"].encode("utf-8")
        if expected_name not in unicode_listing.stdout:
            fail("Unicode source name was not preserved in the archive listing")
        unicode_output = root / "extracted-🧪"
        run(binary, root, "x", "-o", str(unicode_output), str(unicode_archive))
        unicode_restored = unicode_output / "資料-🧪" / "résumé-🧪.txt"
        if not unicode_restored.is_file():
            fail("Unicode archive/source/output paths did not round-trip exactly")
        check_manifest_file(unicode_restored, "unicode")
        print("PASS: names outside the active ACP round-trip through Unicode paths")

        # Wait until the sibling archive stage is receiving source data before
        # changing a byte halfway through the still-open input. This exercises
        # the handle change-time check without relying on a guessed startup
        # delay; a same-size write must fail and leave neither archive nor
        # temporary stage behind.
        race_source = root / "source-race.bin"
        race_archive = root / "source-race.blt"
        with race_source.open("wb") as race_fp:
            race_fp.truncate(256 * 1024 * 1024)
        race_archive_arg = str(race_archive)
        if WINE_RUNNER is not None:
            race_archive_arg = wine_path(race_archive)
        race_command = [str(binary), "c", "-0", race_archive_arg,
                        race_source.name]
        if WINE_RUNNER is not None:
            race_command = [WINE_RUNNER, str(binary), "c", "-0",
                            race_archive_arg, race_source.name]
        race_proc = subprocess.Popen(
            race_command, cwd=root, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=wine_environment() if WINE_RUNNER else None)
        race_stage = None
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and race_proc.poll() is None:
            stages = list(root.glob("bpa*.tmp"))
            if stages and stages[0].stat().st_size >= 1024 * 1024:
                race_stage = stages[0]
                break
            time.sleep(0.001)
        if race_stage is None:
            race_proc.kill()
            race_proc.communicate()
            fail("source-mutation fixture did not observe archive staging")
        with race_source.open("r+b") as race_fp:
            race_fp.seek(128 * 1024 * 1024)
            race_fp.write(b"X")
            race_fp.flush()
        try:
            race_stdout, race_stderr = race_proc.communicate(timeout=90)
        except subprocess.TimeoutExpired:
            race_proc.kill()
            race_proc.communicate()
            fail("source-mutation archive creation timed out")
        if race_proc.returncode == 0:
            fail("Windows accepted a same-size source write during packing")
        if b"source changed while archiving" not in race_stderr:
            fail("source-mutation failure did not report the changed input: "
                 f"{race_stdout!r} {race_stderr!r}")
        if race_archive.exists() or list(root.glob("bpa*.tmp")):
            fail("source-mutation failure published an archive or left a stage")
        print("PASS: same-size source writes during serial packing are refused")

        # Exercise the batch pack path with two files. Point its system temp
        # directory at this fixture so the live tmpfile is visible while the
        # worker reads the large source. The peer ensures this uses a batch of
        # two jobs instead of the single-file serial fallback.
        parallel_source = root / "parallel-source-race.bin"
        parallel_peer = root / "parallel-peer-race.bin"
        parallel_archive = root / "parallel-source-race.blt"
        with parallel_source.open("wb") as parallel_fp:
            parallel_fp.write(os.urandom(64 * 1024 * 1024))
        parallel_peer.write_bytes(os.urandom(1024 * 1024))
        parallel_env = wine_environment()
        if WINE_RUNNER is not None:
            wine_temp = wine_path(root)
            wine_archive = wine_path(parallel_archive)
            wine_binary = wine_path(binary)
            wine_args = [wine_binary, "c", "-F", "-j", "2", wine_archive,
                         parallel_source.name, parallel_peer.name]
            wine_command = (f"set TEMP={wine_temp}&&set TMP={wine_temp}&&" +
                            subprocess.list2cmdline(wine_args))
            parallel_command = [WINE_RUNNER, "cmd.exe", "/c", wine_command]
        else:
            parallel_env["TEMP"] = str(root)
            parallel_env["TMP"] = str(root)
            parallel_command = [str(binary), "c", "-F", "-j", "2",
                                str(parallel_archive), parallel_source.name,
                                parallel_peer.name]
        parallel_proc = subprocess.Popen(
            parallel_command, cwd=root, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=parallel_env)
        parallel_stage = None
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and parallel_proc.poll() is None:
            stages = list(root.glob("t*.tmp"))
            if stages and stages[0].stat().st_size >= 1024 * 1024:
                parallel_stage = stages[0]
                break
            time.sleep(0.001)
        if parallel_stage is None:
            parallel_proc.kill()
            parallel_proc.communicate()
            fail("parallel source-mutation fixture did not observe worker output")
        with parallel_source.open("r+b") as parallel_fp:
            parallel_fp.seek(32 * 1024 * 1024)
            parallel_fp.write(b"X")
            parallel_fp.flush()
        try:
            parallel_stdout, parallel_stderr = parallel_proc.communicate(
                timeout=90)
        except subprocess.TimeoutExpired:
            parallel_proc.kill()
            parallel_proc.communicate()
            fail("parallel source-mutation archive creation timed out")
        if parallel_proc.returncode == 0:
            fail("Windows parallel packing accepted a source write")
        if b"source changed while archiving" not in parallel_stderr:
            fail("parallel source-mutation failure did not report the changed "
                 f"input: {parallel_stdout!r} {parallel_stderr!r}")
        if (parallel_archive.exists() or list(root.glob("t*.tmp")) or
                list(root.glob("bpa*.tmp"))):
            fail("parallel source-mutation failure published an archive or "
                 "left a stage")
        print("PASS: same-size source writes during parallel packing are refused")

        # Wine on Linux exposes the metadata-only source handle through
        # inotify. The first input is recorded before the large directory is
        # enumerated; replace its pathname as soon as that handle closes, so
        # opening the queued job must compare the captured file identity.
        if WINE_RUNNER is not None and os.name != "nt":
            path_race_source = root / "path-race-source.bin"
            path_race_replacement = root / "path-race-replacement.bin"
            path_race_dir = root / "path-race-many"
            path_race_archive = root / "path-race.blt"
            path_race_source.write_bytes(b"original queued payload\n")
            path_race_replacement.write_bytes(b"changed queued payload!\n")
            path_race_dir.mkdir()
            for index in range(4096):
                (path_race_dir / f"member-{index:04d}.txt").write_bytes(b"x")

            libc = ctypes.CDLL(None, use_errno=True)
            inotify_init = libc.inotify_init1
            inotify_init.argtypes = [ctypes.c_int]
            inotify_init.restype = ctypes.c_int
            inotify_add_watch = libc.inotify_add_watch
            inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p,
                                          ctypes.c_uint32]
            inotify_add_watch.restype = ctypes.c_int
            inotify_fd = inotify_init(os.O_NONBLOCK)
            if inotify_fd < 0:
                fail("could not initialize inotify for Wine source path race")
            watch = inotify_add_watch(inotify_fd,
                                      os.fsencode(path_race_source),
                                      0x20 | 0x10)  # IN_OPEN | IN_CLOSE_NOWRITE
            if watch < 0:
                os.close(inotify_fd)
                fail("could not watch the Wine source path race fixture")

            path_race_command = [str(binary), "c", wine_path(path_race_archive),
                                 path_race_source.name, path_race_dir.name]
            path_race_proc = subprocess.Popen(
                [WINE_RUNNER, *path_race_command], cwd=root,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=wine_environment())
            swapped = False
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline and path_race_proc.poll() is None:
                try:
                    events = os.read(inotify_fd, 4096)
                except BlockingIOError:
                    time.sleep(0.001)
                    continue
                offset = 0
                while offset + 16 <= len(events):
                    event_watch, mask, _cookie, name_length = struct.unpack_from(
                        "iIII", events, offset)
                    offset += 16 + name_length
                    if event_watch == watch and mask & 0x10:
                        os.replace(path_race_replacement, path_race_source)
                        swapped = True
                        break
                if swapped:
                    break
            os.close(inotify_fd)
            if not swapped:
                path_race_proc.kill()
                path_race_proc.communicate()
                fail("did not observe source enumeration before path replacement")
            try:
                path_race_stdout, path_race_stderr = path_race_proc.communicate(
                    timeout=90)
            except subprocess.TimeoutExpired:
                path_race_proc.kill()
                path_race_proc.communicate()
                fail("source path-replacement archive creation timed out")
            if path_race_proc.returncode == 0:
                fail("Windows accepted a replaced source path after enumeration")
            if b"source path changed after enumeration" not in path_race_stderr:
                fail("source path-replacement failure did not identify the race: "
                     f"{path_race_stdout!r} {path_race_stderr!r}")
            if path_race_archive.exists() or list(root.glob("bpa*.tmp")):
                fail("source path-replacement failure published an archive or "
                     "left a stage")
            print("PASS: same-size source path replacements after enumeration are refused")

            # Replace a populated parent directory after the first child has
            # been inspected. The enumerator must notice that the directory
            # path no longer names the object it started walking.
            directory_race_path = root / "directory-race-tree"
            directory_race_replacement = root / "directory-race-replacement"
            directory_race_old = root / "directory-race-original"
            directory_race_archive = root / "directory-race.blt"
            directory_race_path.mkdir()
            directory_race_replacement.mkdir()
            for index in range(2048):
                member_name = f"member-{index:04d}.txt"
                (directory_race_path / member_name).write_bytes(b"old")
                (directory_race_replacement / member_name).write_bytes(b"new")
            watched_child = directory_race_path / "member-0000.txt"
            inotify_fd = inotify_init(os.O_NONBLOCK)
            if inotify_fd < 0:
                fail("could not initialize inotify for directory race")
            watch = inotify_add_watch(inotify_fd, os.fsencode(watched_child),
                                      0x20 | 0x10)
            if watch < 0:
                os.close(inotify_fd)
                fail("could not watch the first directory-race child")
            directory_race_proc = subprocess.Popen(
                [WINE_RUNNER, str(binary), "c",
                 wine_path(directory_race_archive),
                 directory_race_path.name],
                cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=wine_environment())
            swapped_directory = False
            deadline = time.monotonic() + 30
            while (time.monotonic() < deadline and
                   directory_race_proc.poll() is None):
                try:
                    events = os.read(inotify_fd, 4096)
                except BlockingIOError:
                    time.sleep(0.001)
                    continue
                offset = 0
                while offset + 16 <= len(events):
                    event_watch, mask, _cookie, name_length = struct.unpack_from(
                        "iIII", events, offset)
                    offset += 16 + name_length
                    if event_watch == watch and mask & 0x10:
                        os.rename(directory_race_path, directory_race_old)
                        os.rename(directory_race_replacement,
                                  directory_race_path)
                        swapped_directory = True
                        break
                if swapped_directory:
                    break
            os.close(inotify_fd)
            if not swapped_directory:
                directory_race_proc.kill()
                directory_race_proc.communicate()
                fail("did not observe child inspection before directory swap")
            try:
                directory_race_stdout, directory_race_stderr = (
                    directory_race_proc.communicate(timeout=90))
            except subprocess.TimeoutExpired:
                directory_race_proc.kill()
                directory_race_proc.communicate()
                fail("parent-directory replacement archive creation timed out")
            if directory_race_proc.returncode == 0:
                fail("Windows accepted a parent-directory replacement during "
                     "enumeration")
            # Depending on where enumeration resumes after the swap, the
            # frontend can detect the changed directory identity directly or
            # fail while inspecting the next child under the replacement path.
            refusal_markers = (b"source directory changed during enumeration",
                               b"cannot inspect source file:", b"cannot stat:")
            if not any(marker in directory_race_stderr
                       for marker in refusal_markers):
                fail("parent-directory race was not reported as an enumeration "
                     f"change: {directory_race_stdout!r} "
                     f"{directory_race_stderr!r}")
            if (directory_race_archive.exists() or
                    list(root.glob("bpa*.tmp"))):
                fail("parent-directory race published an archive or left a stage")
            print("PASS: parent-directory replacements during enumeration are refused")

        pre_epoch_source = root / "pre-epoch.txt"
        pre_epoch_source.write_bytes(b"negative Unix timestamps are unknown\n")
        os.utime(pre_epoch_source, (-1, -1))
        pre_epoch_archive = root / "pre-epoch.blt"
        run(binary, root, "c", "-0", str(pre_epoch_archive),
            pre_epoch_source.name)
        archived_time, archived_flags = primary_entry_times(pre_epoch_archive)[
            pre_epoch_source.name]
        if archived_time != 0 or archived_flags & 0x4000:
            fail("pre-1970 source time was not stored as unknown in format 6")
        print("PASS: pre-1970 source time is encoded as unknown")

        corrupt_source = root / "src" / "corrupt.bin"
        corrupt_source.parent.mkdir()
        corrupt_source.write_bytes(bytes(range(256)) * 1024)
        corrupt_archive = root / "corrupt.blt"
        run(binary, root, "c", "-0", str(corrupt_archive),
            corrupt_source.relative_to(root).as_posix())
        damaged = bytearray(corrupt_archive.read_bytes())
        if len(damaged) < 96:
            fail("Stored corruption fixture is unexpectedly short")
        entry_start = 32
        header_size = int.from_bytes(damaged[entry_start:entry_start + 2], "little")
        name_size = int.from_bytes(damaged[entry_start + 2:entry_start + 4], "little")
        payload_start = entry_start + header_size
        block_size = int.from_bytes(
            damaged[entry_start + name_size + 26:
                    entry_start + name_size + 30], "little")
        if block_size:
            payload_start += 12
        if payload_start >= len(damaged) - 32:
            fail("Stored corruption fixture has no data payload")
        damaged[payload_start] ^= 1
        corrupt_archive.write_bytes(damaged)
        corrupt_output = root / "corrupt-output"
        existing = corrupt_output / "src" / "corrupt.bin"
        existing.parent.mkdir(parents=True)
        sentinel = b"existing Windows target must survive corrupt extraction\n"
        existing.write_bytes(sentinel)
        check_manifest_file(existing, "corrupt_destination_sentinel")
        corrupt_result = run(binary, root, "x", "-f", "-o", str(corrupt_output),
                             str(corrupt_archive), success=False)
        if b"checksum" not in corrupt_result.stderr.lower():
            fail("corruption fixture failed before the block checksum path")
        check_manifest_file(existing, "corrupt_destination_sentinel")
        if any(path.is_file() and path.name.lower().startswith("bp") and
               path.suffix.lower() == ".tmp"
               for path in corrupt_output.rglob("*")):
            fail("corrupt extraction left a staged temporary file")
        print("PASS: corrupt extraction preserves the existing file and cleans its stage")

        metadata_source = root / "metadata-src"
        metadata_source.mkdir()
        metadata_file = metadata_source / "hidden-system.bin"
        metadata_file.write_bytes(b"portable FAT attributes round-trip\n")
        check_manifest_file(metadata_file, "metadata")
        set_attributes(metadata_file, "H", "S")
        if not has_attributes(metadata_file, "H", "S"):
            fail("could not set source HIDDEN and SYSTEM attributes")
        file_mtime = EXPECTED["metadata"]["file_mtime_unix"]
        directory_mtime = EXPECTED["metadata"]["directory_mtime_unix"]
        os.utime(metadata_file, (file_mtime, file_mtime))
        metadata_archive = root / "metadata.blt"
        run(binary, root, "c", "-0", str(metadata_archive),
            metadata_file.relative_to(root).as_posix())
        metadata_output = root / "metadata-output"
        run(binary, root, "x", "-o", str(metadata_output), str(metadata_archive))
        restored_metadata_file = metadata_output / "metadata-src" / metadata_file.name
        if not restored_metadata_file.is_file():
            fail("file data did not round-trip with portable FAT metadata")
        check_manifest_file(restored_metadata_file, "metadata")
        if not has_attributes(restored_metadata_file, "H", "S"):
            fail("portable HIDDEN and SYSTEM attributes were not restored")
        if int(restored_metadata_file.stat().st_mtime) != file_mtime:
            fail("file modification timestamp was not restored")
        print("PASS: file HIDDEN and SYSTEM flags round-trip through archive metadata")

        if WINE_RUNNER is not None and args.linux_cli.is_file():
            linux_cli = args.linux_cli.resolve()
            linux_test = subprocess.run(
                [str(linux_cli), "t", str(metadata_archive)], cwd=root,
                capture_output=True,
            )
            if linux_test.returncode != 0:
                fail(f"Linux CLI rejected the Windows-created archive: {linux_test.stderr!r}")
            linux_output = root / "linux-output"
            linux_extract = subprocess.run(
                [str(linux_cli), "x", "-o", str(linux_output),
                 str(metadata_archive)], cwd=root, capture_output=True,
            )
            if (linux_extract.returncode == 0 or
                    b"metadata" not in linux_extract.stdout.lower() + linux_extract.stderr.lower()):
                fail("Linux did not report Windows FAT metadata as unsupported")
            linux_restored = linux_output / "metadata-src" / metadata_file.name
            if not linux_restored.is_file():
                fail("Linux did not extract the Windows-created file bytes")
            check_manifest_file(linux_restored, "metadata")
            print("PASS: Linux verifies Windows archive and extracts data with metadata partial")

            linux_archive = root / "linux-created.blt"
            linux_create = subprocess.run(
                [str(linux_cli), "c", "-0", str(linux_archive),
                 metadata_file.relative_to(root).as_posix()], cwd=root,
                capture_output=True,
            )
            if linux_create.returncode != 0:
                fail(f"Linux CLI could not create the cross-transfer fixture: {linux_create.stderr!r}")
            run(binary, root, "t", str(linux_archive))
            windows_output = root / "windows-output"
            windows_extract = run(binary, root, "x", "-o", str(windows_output),
                                  str(linux_archive), success=False)
            if b"partial success" not in windows_extract.stderr.lower():
                fail("Windows did not report unsupported Linux mode metadata as partial")
            windows_restored = windows_output / "metadata-src" / metadata_file.name
            if not windows_restored.is_file():
                fail("Windows did not extract the Linux-created archive bytes")
            check_manifest_file(windows_restored, "metadata")
            print("PASS: Windows tests and extracts Linux-created v6 archive")

            profile_archive = root / "windows-profile-1.blt"
            profile_source = root / "profile-compressible.bin"
            profile_source.write_bytes(bytes(range(256)) * 1024)
            check_manifest_file(profile_source, "profile_payload")
            run(binary, root, "c", "--interchange-profile-1",
                str(profile_archive), profile_source.relative_to(root).as_posix())
            profile_entries = primary_entry_properties(profile_archive)
            if profile_entries.get("profile-compressible.bin") != (1, 16384):
                fail(f"Windows profile output is not BoltLite/16 KiB: {profile_entries!r}")
            profile_full_test = subprocess.run(
                [str(linux_cli), "t", str(profile_archive)], cwd=root,
                capture_output=True,
            )
            if profile_full_test.returncode != 0:
                fail(f"Linux full reader rejected the Windows profile archive: {profile_full_test.stderr!r}")
            if args.linux_mini.is_file():
                profile_mini_test = subprocess.run(
                    [args.mini_runner, str(args.linux_mini.resolve()), "t",
                     str(profile_archive)], cwd=root, capture_output=True,
                )
                if profile_mini_test.returncode != 0:
                    fail(f"Linux mini reader rejected the Windows profile archive: {profile_mini_test.stderr!r}")
                print("PASS: Windows Profile 1 writes BoltLite/16 KiB accepted by full and mini readers")
            else:
                print("SKIP: Linux mini reader is unavailable for Windows profile check")

            mapping_source = root / "mapping-source"
            mapping_source.mkdir()
            reserved_source = mapping_source / "CON.txt"
            reserved_source.write_bytes(b"reserved Windows device name\n")
            reserved_archive = root / "reserved-name.blt"
            reserved_create = subprocess.run(
                [str(linux_cli), "c", "-0", str(reserved_archive),
                 reserved_source.relative_to(root).as_posix()], cwd=root,
                capture_output=True,
            )
            if reserved_create.returncode != 0:
                fail("Linux could not create the reserved-name fixture")
            reserved_output = root / "reserved-output"
            reserved_extract = run(binary, root, "x", "-o", str(reserved_output),
                                   str(reserved_archive), success=False)
            if reserved_output.exists():
                fail("Windows created an output tree for a reserved device name")
            if b"unsupported Windows filename" not in reserved_extract.stderr:
                fail("reserved Windows name was not rejected during preflight")

            for invalid_index, invalid_name in enumerate(("trailing.", "trailing ")):
                invalid_source = mapping_source / invalid_name
                invalid_source.write_bytes(b"Windows trims this suffix\n")
                invalid_archive = root / ("invalid-name-" + str(invalid_index) + ".blt")
                invalid_create = subprocess.run(
                    [str(linux_cli), "c", "-0", str(invalid_archive),
                     invalid_source.relative_to(root).as_posix()], cwd=root,
                    capture_output=True,
                )
                if invalid_create.returncode != 0:
                    fail("Linux could not create a trailing-dot/space fixture")
                invalid_output = root / ("invalid-output-" + str(invalid_index))
                invalid_extract = run(binary, root, "x", "-o", str(invalid_output),
                                      str(invalid_archive), success=False)
                if (invalid_output.exists() or
                        b"unsupported Windows filename" not in invalid_extract.stderr):
                    fail(f"Windows did not preflight-refuse {invalid_name!r}")

            upper_source = mapping_source / "Alias.txt"
            lower_source = mapping_source / "alias.txt"
            upper_source.write_bytes(b"upper spelling\n")
            lower_source.write_bytes(b"lower spelling\n")
            collision_archive = root / "case-collision.blt"
            collision_create = subprocess.run(
                [str(linux_cli), "c", "-0", str(collision_archive),
                 upper_source.relative_to(root).as_posix(),
                 lower_source.relative_to(root).as_posix()], cwd=root,
                capture_output=True,
            )
            if collision_create.returncode != 0:
                fail("Linux could not create the case-collision fixture")
            collision_output = root / "collision-output"
            collision_extract = run(binary, root, "x", "-o", str(collision_output),
                                    str(collision_archive), success=False)
            if collision_output.exists():
                fail("Windows created an output tree before rejecting case aliases")
            if b"destination name collision" not in collision_extract.stderr:
                fail("case-insensitive filename collision was not rejected in preflight")

            prefix_file_root = root / "prefix-file-root"
            prefix_child_root = root / "prefix-child-root"
            (prefix_file_root / "parent").mkdir(parents=True)
            (prefix_file_root / "parent" / "child.txt").write_bytes(b"child\n")
            prefix_child_root.mkdir()
            (prefix_child_root / "parent").write_bytes(b"parent file\n")

            file_then_child = root / "file-then-child.blt"
            created = subprocess.run(
                [str(linux_cli), "c", "-0", str(file_then_child),
                 "parent"], cwd=prefix_child_root, capture_output=True,
            )
            appended = subprocess.run(
                [str(linux_cli), "a", "-0", str(file_then_child),
                 "parent/child.txt"], cwd=prefix_file_root, capture_output=True,
            )
            if created.returncode != 0 or appended.returncode != 0:
                fail("Linux could not create the file-then-child path fixture")
            prefix_output = root / "prefix-output"
            prefix_extract = run(binary, root, "x", "-o", str(prefix_output),
                                 str(file_then_child), success=False)
            if prefix_output.exists() or b"destination name collision" not in prefix_extract.stderr:
                fail("file/child prefix conflict was not refused before output changes")

            child_then_file = root / "child-then-file.blt"
            created = subprocess.run(
                [str(linux_cli), "c", "-0", str(child_then_file),
                 "parent/child.txt"], cwd=prefix_file_root, capture_output=True,
            )
            appended = subprocess.run(
                [str(linux_cli), "a", "-0", str(child_then_file),
                 "parent"], cwd=prefix_child_root, capture_output=True,
            )
            if created.returncode != 0 or appended.returncode != 0:
                fail("Linux could not create the child-then-file path fixture")
            reverse_prefix_output = root / "reverse-prefix-output"
            reverse_prefix_extract = run(
                binary, root, "x", "-o", str(reverse_prefix_output),
                str(child_then_file), success=False,
            )
            if (reverse_prefix_output.exists() or
                    b"destination name collision" not in reverse_prefix_extract.stderr):
                fail("child/file prefix conflict was not refused before output changes")

            sibling_root = root / "prefix-sibling-root"
            sibling_root.mkdir()
            (sibling_root / "parent-foo").write_bytes(b"sibling\n")
            sibling_order = root / "sibling-prefix-order.blt"
            created = subprocess.run(
                [str(linux_cli), "c", "-0", str(sibling_order),
                 "parent/child.txt"], cwd=prefix_file_root, capture_output=True,
            )
            sibling_append = subprocess.run(
                [str(linux_cli), "a", "-0", str(sibling_order),
                 "parent-foo"], cwd=sibling_root, capture_output=True,
            )
            parent_append = subprocess.run(
                [str(linux_cli), "a", "-0", str(sibling_order),
                 "parent"], cwd=prefix_child_root, capture_output=True,
            )
            if (created.returncode != 0 or sibling_append.returncode != 0 or
                    parent_append.returncode != 0):
                fail("Linux could not create the sibling-prefix ordering fixture")
            sibling_output = root / "sibling-prefix-output"
            sibling_extract = run(binary, root, "x", "-o", str(sibling_output),
                                  str(sibling_order), success=False)
            if (sibling_output.exists() or
                    b"destination name collision" not in sibling_extract.stderr):
                fail("a similar sibling name hid a file/directory prefix conflict")
            print("PASS: reserved names, aliases, and file/directory prefixes are refused before output")

        parallel_peer = metadata_source / "parallel-peer.bin"
        parallel_peer.write_bytes(bytes(range(128)) * 256)
        check_manifest_file(parallel_peer, "parallel_peer")
        os.utime(parallel_peer, (-1, -1))
        set_attributes(metadata_source, "H", "S")
        os.utime(metadata_source, (directory_mtime, directory_mtime))
        parallel_archive = root / "parallel-metadata.blt"
        run(binary, root, "c", "-F", "-j", "2", str(parallel_archive),
            metadata_source.relative_to(root).as_posix())
        parallel_mtime, parallel_flags = primary_entry_times(parallel_archive)[
            "metadata-src/parallel-peer.bin"]
        if parallel_mtime != 0 or parallel_flags & 0x4000:
            fail("parallel packing did not store a pre-1970 source time as unknown")
        parallel_output = root / "parallel-output"
        run(binary, root, "x", "-o", str(parallel_output),
            str(parallel_archive))
        parallel_restored = parallel_output / "metadata-src" / metadata_file.name
        if not has_attributes(parallel_restored, "H", "S"):
            fail("parallel create did not preserve portable HIDDEN and SYSTEM metadata")
        restored_metadata_dir = parallel_output / metadata_source.name
        if not has_attributes(restored_metadata_dir, "H", "S"):
            fail("deferred portable directory HIDDEN and SYSTEM attributes were not restored")
        if int(restored_metadata_dir.stat().st_mtime) != directory_mtime:
            fail("deferred directory modification timestamp was not restored")
        check_manifest_file(parallel_output / "metadata-src" / parallel_peer.name,
                            "parallel_peer")
        print("PASS: parallel Fluff creation writes portable FAT metadata groups")

        source = root / "src"
        source.mkdir(exist_ok=True)
        (source / "original.txt").write_bytes(b"original archive member\n")
        (source / "added.txt").write_bytes(b"new member after append\n")
        check_manifest_file(source / "original.txt", "original_member")
        check_manifest_file(source / "added.txt", "added_member")
        archive = root / "archive.bolt"

        run(binary, root, "c", str(archive), "src/original.txt")
        set_attributes(archive, "H", "S")
        if not has_attributes(archive, "H", "S"):
            fail("could not set FILE_ATTRIBUTE_HIDDEN/SYSTEM on the archive")
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

        if WINE_RUNNER is not None:
            print("SKIP: Wine does not expose POSIX symlinks as Windows reparse points")
        else:
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

        read_only_bytes = archive.read_bytes()
        set_attributes(archive, "R")
        if not has_attributes(archive, "R", "H", "S"):
            fail("could not set read-only archive attributes")
        for command, member in (("a", "src/added.txt"),
                                ("d", "src/original.txt")):
            read_only_result = run(binary, root, command, str(archive), member,
                                   success=False)
            if b"writable" not in read_only_result.stderr.lower():
                fail(f"read-only archive {command} was not refused in preflight")
            if archive.read_bytes() != read_only_bytes or not has_attributes(
                    archive, "R", "H", "S"):
                fail(f"read-only archive {command} changed bytes or attributes")
            if any(path.is_file() and path.name.lower().startswith("bp") and
                   path.suffix.lower() == ".tmp" for path in root.iterdir()):
                fail(f"read-only archive {command} left a staged temporary file")
        remove_attributes(archive, "R")
        if not has_attributes(archive, "H", "S"):
            fail("clearing READONLY changed the archive's HIDDEN/SYSTEM flags")
        print("PASS: append/delete refuse read-only archive targets unchanged")

        run(binary, root, "a", str(archive), "src/added.txt")
        run(binary, root, "t", str(archive))
        check_archive_attributes_after_edit(archive, "append")
        listing = run(binary, root, "l", str(archive))
        if b"original.txt" not in listing.stdout or b"added.txt" not in listing.stdout:
            fail("append listing is missing an expected entry")
        append_output = root / "append-output"
        run(binary, root, "x", "-o", str(append_output), str(archive))
        check_manifest_file(append_output / "src" / "original.txt", "original_member")
        check_manifest_file(append_output / "src" / "added.txt", "added_member")
        print("PASS: append publishes a readable archive")

        run(binary, root, "d", str(archive), "src/original.txt")
        run(binary, root, "t", str(archive))
        check_archive_attributes_after_edit(archive, "delete")
        listing = run(binary, root, "l", str(archive))
        if b"original.txt" in listing.stdout or b"added.txt" not in listing.stdout:
            fail("delete listing does not match the requested edit")
        delete_output = root / "delete-output"
        run(binary, root, "x", "-o", str(delete_output), str(archive))
        if (delete_output / "src" / "original.txt").exists():
            fail("delete retained the removed member")
        check_manifest_file(delete_output / "src" / "added.txt", "added_member")
        print("PASS: delete publishes a readable archive")

    print("Wine Win64 archive checks passed" if WINE_RUNNER is not None
          else "Windows archive edit checks passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
