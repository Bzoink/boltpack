#!/usr/bin/env python3
"""Build the macOS POSIX frontend on the host and exercise transfer rules.

This qualifies the shared POSIX path and archive logic on the host. It does
not exercise Apple resource forks, Finder xattrs, or the macOS target APIs.
"""

import argparse
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run(args, cwd, expected=0, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode != expected:
        raise AssertionError(
            f"command {args!r} returned {result.returncode}, expected {expected}\n"
            f"stdout: {result.stdout.decode(errors='replace')}\n"
            f"stderr: {result.stderr.decode(errors='replace')}"
        )
    return result


def build_host_cli(cc, output):
    command = shlex.split(cc) + [
        "-U__linux__",
        "-ansi", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-function",
        "-O2", "-DBP_VERSION_MAJOR=1", "-DBP_VERSION_MINOR=9",
        "-I" + str(ROOT / "src/core"),
        str(ROOT / "src/core/archiver.c"),
        str(ROOT / "src/core/compress.c"),
        str(ROOT / "src/core/boltlite.c"),
        str(ROOT / "src/core/fluff.c"),
        str(ROOT / "src/core/bp_codepage.c"),
        str(ROOT / "src/frontend/macosx/main.c"),
        str(ROOT / "src/core/boltlz.c"),
        "-o", str(output),
    ]
    subprocess.run(command, cwd=ROOT, check=True)


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


def check_apple_branches_syntax(cc):
    command = shlex.split(cc) + [
        "-fsyntax-only", "-U__linux__", "-D__APPLE__",
        "-ansi", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-function",
        "-O2", "-DBP_VERSION_MAJOR=1", "-DBP_VERSION_MINOR=9",
        "-I" + str(ROOT / "test/macosx_stubs"),
        "-I" + str(ROOT / "src/core"),
        str(ROOT / "src/frontend/macosx/main.c"),
    ]
    subprocess.run(command, cwd=ROOT, check=True)


def build_rename_failure_interposer(cc, output, source):
    source.write_text(r'''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int (*real_rename_fn)(const char *, const char *);
int rename(const char *old_path, const char *new_path)
{
    const char *target = getenv("BP_TEST_RENAME_TARGET");
    const char *marker = getenv("BP_TEST_RENAME_MARKER");
    int fd;
    if (target != NULL && strcmp(target, new_path) == 0) {
        if (marker != NULL) {
            fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) close(fd);
        }
        errno = EIO;
        return -1;
    }
    if (real_rename_fn == NULL)
        *(void **)(&real_rename_fn) = dlsym(RTLD_NEXT, "rename");
    if (real_rename_fn == NULL) { errno = EIO; return -1; }
    return real_rename_fn(old_path, new_path);
}
''', encoding="ascii")
    subprocess.run(shlex.split(cc) + ["-shared", "-fPIC", str(source),
                                      "-ldl", "-o", str(output)],
                   cwd=ROOT, check=True)


def check_edit_publication_failure(binary, root, archive, operation, args,
                                   interposer):
    marker = root / (operation + "-rename-called")
    marker.unlink(missing_ok=True)
    before = archive.read_bytes()
    env = os.environ.copy()
    prior_preload = env.get("LD_PRELOAD")
    env["LD_PRELOAD"] = str(interposer) + (
        ":" + prior_preload if prior_preload else "")
    env["BP_TEST_RENAME_TARGET"] = str(archive)
    env["BP_TEST_RENAME_MARKER"] = str(marker)
    run([str(binary), operation, str(archive), *args], root, expected=1,
        env=env)
    assert marker.exists(), f"{operation} did not reach failed publication"
    assert archive.read_bytes() == before, f"failed {operation} changed archive"
    assert not list(root.glob(archive.name + ".bp_tmp.*")), \
        f"failed {operation} left a sibling stage"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--linux-boltpack", default=str(ROOT / "build/linux/boltpack"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="boltpack-macosx-host-") as temp:
        root = Path(temp)
        mode_test = root / "test-posix-restore-mode"
        subprocess.run(shlex.split(args.cc) + [
            "-ansi", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-function",
            "-I" + str(ROOT / "src/core"),
            "-I" + str(ROOT / "src/frontend/posix"),
            str(ROOT / "test/unit/test_posix_restore_mode.c"),
            "-o", str(mode_test)], cwd=ROOT, check=True)
        run([str(mode_test)], root)

        binary = root / "boltpack-macosx-host"
        check_apple_branches_syntax(args.cc)
        build_host_cli(args.cc, binary)

        # Profile create, append, test, and delete all operate on staged files.
        (root / "first.txt").write_bytes(b"first archive member\n")
        (root / "second.txt").write_bytes(b"second archive member\n")
        run([str(binary), "c", "--interchange-profile-1", "profile.bolt",
             "first.txt"], root)
        run([str(binary), "a", "--interchange-profile-1", "profile.bolt",
             "second.txt"], root)
        run([str(binary), "t", "profile.bolt"], root)
        run([str(binary), "d", "profile.bolt", "first.txt"], root)
        run([str(binary), "t", "profile.bolt"], root)
        listing = run([str(binary), "l", "profile.bolt"], root).stdout
        assert b"second.txt" in listing and b"first.txt" not in listing

        # A pre-epoch POSIX mtime is unknown in format 6, never wrapped.
        pre_epoch = root / "pre-epoch.txt"
        pre_epoch.write_bytes(b"macOS host pre-epoch timestamp\n")
        os.utime(pre_epoch, (-1, -1))
        pre_epoch_archive = root / "pre-epoch.bolt"
        run([str(binary), "c", "-0", str(pre_epoch_archive),
             pre_epoch.name], root)
        mtime, flags = primary_mtimes(pre_epoch_archive)[pre_epoch.name]
        assert mtime == 0 and not (flags & 0x4000), \
            "macOS frontend marked pre-epoch time as valid"
        run([str(binary), "t", str(pre_epoch_archive)], root)

        # Failed append/delete publication preserves the prior archive bytes.
        if sys.platform.startswith("linux"):
            edit_archive = root / "edit.bolt"
            run([str(binary), "c", str(edit_archive), "first.txt"], root)
            interposer = root / "fail-rename.so"
            build_rename_failure_interposer(args.cc, interposer,
                                            root / "fail-rename.c")
            check_edit_publication_failure(binary, root, edit_archive, "a",
                                           ["second.txt"], interposer)
            run([str(binary), "a", str(edit_archive), "second.txt"], root)
            check_edit_publication_failure(binary, root, edit_archive, "d",
                                           ["first.txt"], interposer)
            run([str(binary), "t", str(edit_archive)], root)
        else:
            print("SKIP: rename failure injection requires Linux LD_PRELOAD")

        # File/directory attributes, including valid Unix epoch zero, round-trip.
        source = root / "source"
        source.mkdir()
        payload = b"format six host lifecycle\n"
        source_file = source / "epoch.txt"
        source_file.write_bytes(payload)
        source_file.chmod(0o640)
        os.utime(source_file, (0, 0))
        source.chmod(0o750)
        os.utime(source, (0, 0))
        run([str(binary), "c", "-0", "metadata.bolt", "source"], root)
        run([str(binary), "t", "metadata.bolt"], root)

        dry = root / "dry-run-output"
        run([str(binary), "x", "--dry-run", "-o", str(dry), "metadata.bolt"], root)
        assert not dry.exists(), "dry-run created the destination tree"

        extracted = root / "extracted"
        run([str(binary), "x", "-o", str(extracted), "metadata.bolt"], root)
        output_file = extracted / "source/epoch.txt"
        output_dir = extracted / "source"
        assert output_file.read_bytes() == payload
        assert output_file.stat().st_mode & 0o777 == 0o640
        assert int(output_file.stat().st_mtime) == 0
        assert output_dir.stat().st_mode & 0o777 == 0o750
        assert int(output_dir.stat().st_mtime) == 0

        # An existing file is preserved on a no-overwrite skip.
        output_file.write_bytes(b"existing content\n")
        run([str(binary), "x", "-o", str(extracted), "metadata.bolt"],
            root, expected=1)
        assert output_file.read_bytes() == b"existing content\n"

        # A bad payload checksum never publishes a stage over an existing file.
        bad_source = root / "bad.bin"
        bad_payload = bytes((i * 37 + 11) & 0xFF for i in range(4096))
        bad_source.write_bytes(bad_payload)
        run([str(binary), "c", "-0", "corrupt.bolt", "bad.bin"], root)
        corrupt_path = root / "corrupt.bolt"
        corrupt = bytearray(corrupt_path.read_bytes())
        payload_offset = corrupt.find(bad_payload)
        assert payload_offset >= 0, "stored fixture payload was not found"
        corrupt[payload_offset + 128] ^= 0x80
        corrupt_path.write_bytes(corrupt)
        corrupt_out = root / "corrupt-out"
        corrupt_out.mkdir()
        preserved = corrupt_out / "bad.bin"
        preserved.write_bytes(b"original target\n")
        run([str(binary), "x", "-f", "-o", str(corrupt_out),
             "corrupt.bolt"], root, expected=1)
        assert preserved.read_bytes() == b"original target\n"
        assert not list(corrupt_out.rglob(".boltpack-*")), "failed stage remains"

        # Case-fold collisions are rejected before creating the destination.
        linux = Path(args.linux_boltpack).resolve()
        assert linux.is_file(), f"Linux fixture writer not found: {linux}"
        (root / "Case.txt").write_text("upper\n", encoding="ascii")
        (root / "case.txt").write_text("lower\n", encoding="ascii")
        run([str(linux), "c", "case-collision.bolt", "Case.txt", "case.txt"], root)
        collision_out = root / "collision-out"
        run([str(binary), "x", "-o", str(collision_out), "case-collision.bolt"],
            root, expected=1)
        assert not collision_out.exists(), "collision preflight changed destination"

        # Existing symlink parents cannot redirect extraction outside the root.
        nested = root / "nested"
        nested.mkdir()
        (nested / "file.txt").write_text("safe\n", encoding="ascii")
        run([str(linux), "c", "nested.bolt", "nested"], root)
        outside = root / "outside"
        outside.mkdir()
        symlink_out = root / "symlink-out"
        symlink_out.mkdir()
        (symlink_out / "nested").symlink_to(outside, target_is_directory=True)
        run([str(binary), "x", "-o", str(symlink_out), "nested.bolt"],
            root, expected=1)
        assert not (outside / "file.txt").exists(), "extraction escaped via symlink"

        # Exercise the directory-relative extraction path and staged symlink
        # publication used by the macOS frontend, using host libc APIs.
        subprocess.run([sys.executable, str(ROOT / "test/check_extract_race.py"),
                        "--boltpack", str(binary)], cwd=ROOT, check=True)
        subprocess.run([sys.executable, str(ROOT / "test/check_symlink_atomic.py"),
                        "--boltpack", str(binary), "--cc", args.cc],
                       cwd=ROOT, check=True)

    print("macOS POSIX host lifecycle checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
