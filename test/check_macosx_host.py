#!/usr/bin/env python3
"""Build the macOS POSIX frontend on the host and exercise transfer rules.

This qualifies the shared POSIX path and archive logic on the host. It does
not exercise Apple resource forks, Finder xattrs, or the macOS target APIs.
"""

import argparse
import atexit
import contextlib
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
RUN_DIR = None
RUN_STATE = "FAIL: runner exited before all checks completed"
RUN_ERROR = None
COMMANDS = []


def _path_text(path):
    return os.fsdecode(os.fspath(path))


def recorded_run(command, **kwargs):
    check = kwargs.pop("check", False)
    result = None
    try:
        result = subprocess.run(command, check=False, **kwargs)
        return result
    finally:
        entry = {
            "command": [_path_text(part) for part in command],
            "cwd": _path_text(kwargs["cwd"]) if kwargs.get("cwd") else None,
        }
        env = kwargs.get("env")
        if env is not None:
            entry["environment"] = {
                key: env.get(key, "<unset>")
                for key in ("LD_PRELOAD", "BP_TEST_RENAME_TARGET",
                            "BP_TEST_RENAME_MARKER", "TZ", "LANG",
                            "LC_ALL")
            }
        if result is not None:
            entry.update({
                "exit_code": result.returncode,
                "stdout": result.stdout.decode("utf-8", "replace")
                if isinstance(result.stdout, bytes) else result.stdout,
                "stderr": result.stderr.decode("utf-8", "replace")
                if isinstance(result.stderr, bytes) else result.stderr,
            })
        COMMANDS.append(entry)
        if check and result is not None and result.returncode:
            raise subprocess.CalledProcessError(
                result.returncode, command, output=result.stdout,
                stderr=result.stderr)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_run_record():
    if RUN_DIR is None:
        return
    (RUN_DIR / "command-log.jsonl").write_text(
        "".join(json.dumps(item, sort_keys=True) + "\n" for item in COMMANDS),
        encoding="utf-8")
    status = subprocess.check_output(["git", "status", "--short"], cwd=ROOT,
                                     text=True).splitlines()
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"],
                                   cwd=ROOT)
    (RUN_DIR / "tracked-diff.patch").write_bytes(diff)
    source_paths = [
        ROOT / "Makefile",
        ROOT / "test/check_macosx_host.py",
        write_run_record.manifest,
        ROOT / "src/frontend/macosx/main.c",
        ROOT / "src/frontend/posix/path.h",
        ROOT / "test/macosx_stubs/sys/xattr.h",
        ROOT / "test/unit/test_posix_restore_mode.c",
        ROOT / "src/core/archiver.c",
        ROOT / "src/core/compress.c",
        ROOT / "src/core/boltlite.c",
        ROOT / "src/core/boltlz.c",
        ROOT / "src/core/fluff.c",
        ROOT / "src/core/bp_codepage.c",
    ]
    artifacts = {}
    for path in sorted(RUN_DIR.rglob("*")):
        if path.is_file() and path.name != "run.json":
            artifacts[path.relative_to(RUN_DIR).as_posix()] = {
                "size": path.stat().st_size,
                "sha256": sha256(path),
            }
    compiler = getattr(write_run_record, "compiler", "cc")
    compiler_version = subprocess.run(shlex.split(compiler) + ["--version"],
                                      capture_output=True, text=True,
                                      check=False)
    filesystem_name = "POSIX host filesystem (type not queried)"
    if sys.platform.startswith("linux"):
        filesystem = subprocess.run(
            ["stat", "-f", "--format=%T", str(RUN_DIR)],
            capture_output=True, text=True, check=False)
        if filesystem.returncode == 0 and filesystem.stdout.strip():
            filesystem_name = filesystem.stdout.strip()
    executable_paths = {
        "macos_posix_host_cli": getattr(write_run_record, "binary", None),
        "linux_fixture_cli": getattr(write_run_record, "linux_cli", None),
    }
    record = {
        "purpose": "macOS POSIX CLI host-shim lifecycle and interoperability checks",
        "date": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "result": RUN_STATE,
        "error": RUN_ERROR,
        "environment": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "compiler": compiler,
            "compiler_version": compiler_version.stdout.splitlines()[0]
            if compiler_version.stdout else "unknown",
            "filesystem": filesystem_name,
            "locale": {key: os.environ.get(key, "<unset>")
                       for key in ("LANG", "LC_ALL", "LC_CTYPE")},
            "timezone": os.environ.get("TZ", "<unset>"),
            "scope": "Host POSIX libc and Apple API syntax stubs; no macOS runtime, xattrs, resource forks, or Finder metadata qualification",
        },
        "compiler_flags": [
            "-U__linux__", "-ansi", "-pedantic", "-Wall", "-Wextra",
            "-Wno-unused-function", "-O2", "-DBP_VERSION_MAJOR=1",
            "-DBP_VERSION_MINOR=9",
        ],
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                              cwd=ROOT, text=True).strip(),
        "worktree_status": status,
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "independent_manifest": {
            "path": "test/reference/interoperability/macosx-host.json",
            "sha256": sha256(ROOT / "test/reference/interoperability/macosx-host.json"),
        },
        "commands": len(COMMANDS),
        "command_log": "command-log.jsonl",
        "sources": {path.relative_to(ROOT).as_posix(): sha256(path)
                    for path in source_paths if path.is_file()},
        "executables": {
            name: {"path": str(path), "size": path.stat().st_size,
                   "sha256": sha256(path)}
            for name, path in executable_paths.items()
            if path is not None and path.is_file()
        },
        "missing_executables": [name for name, path in executable_paths.items()
                                if path is None or not path.is_file()],
        "cases": [
            "Profile 1 create/append/test/delete and listing",
            "macos-modern-v1 create preflight; ASCII case collision append preserves archive",
            "macos-classic-hfs-v1 MacRoman projection, component limits, collision, metadata, codec, and append checks",
            "atari-st Atari GEMDOS projection, case collision, high-bit refusal, metadata omission, codec, and append checks",
            "sinclair-ql-latin1-v1 Latin-1 projection, slash alias collision, path and codec limits, metadata omission, and collision-safe append",
            "riscos-latin1-v1 Latin-1 projection, FileCore syntax/component/path limits, case alias, metadata refusal, and codec checks",
            "amigaos-68k-latin1 projection, component/path limits, collision, metadata, codec, and append checks",
            "DOS-family CP437/CP850 8.3 and mini profiles, append, metadata, and refusal checks",
            "pre-epoch time becomes unknown; epoch-zero file and directory mode/time restore",
            "dry-run, no-overwrite, corrupt-checksum preservation, and symlink-parent refusal",
            "edit publication failures preserve the old archive and clean stages",
            "case-fold collision preflight; Linux-created archive cross-read",
            "directory-relative extraction and atomic staged symlink helper checks",
            "Apple API branch syntax checked with host stubs",
        ],
        "artifacts": artifacts,
    }
    (RUN_DIR / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                       encoding="utf-8")


def record_unhandled_exception(exc_type, exc, traceback):
    global RUN_ERROR
    RUN_ERROR = "%s: %s" % (exc_type.__name__, exc)
    sys.__excepthook__(exc_type, exc, traceback)


def run(args, cwd, expected=0, env=None):
    result = recorded_run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
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
    recorded_run(command, cwd=ROOT, check=True, capture_output=True)


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


def archive_entries(path):
    data = Path(path).read_bytes()
    table_offset = struct.unpack_from("<Q", data, len(data) - 20)[0]
    count = struct.unpack_from("<I", data, table_offset + 4)[0]
    record = table_offset + 16
    entries = []
    for _ in range(count):
        name_length = struct.unpack_from("<H", data, record)[0]
        name = data[record + 2:record + 2 + name_length].decode("utf-8")
        fields = record + 2 + name_length
        entries.append((name, data[fields + 16]))
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
    recorded_run(command, cwd=ROOT, check=True, capture_output=True)


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
    recorded_run(shlex.split(cc) + ["-shared", "-fPIC", str(source),
                                    "-ldl", "-o", str(output)],
                 cwd=ROOT, check=True, capture_output=True)


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
    parser.add_argument("--manifest", type=Path,
                        default=ROOT / "test/reference/interoperability/macosx-host.json")
    parser.add_argument("--output-root", type=Path,
                        default=ROOT / "build/macosx-host/runs")
    args = parser.parse_args()
    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    global RUN_DIR
    RUN_DIR = Path(tempfile.mkdtemp(prefix="macosx-host-", dir=output_root))
    write_run_record.compiler = args.cc
    write_run_record.binary = RUN_DIR / "boltpack-macosx-host"
    write_run_record.linux_cli = Path(args.linux_boltpack).resolve()
    write_run_record.manifest = manifest_path
    atexit.register(write_run_record)
    sys.excepthook = record_unhandled_exception
    (RUN_DIR / "macosx-host.json").write_bytes(manifest_path.read_bytes())

    for name, record in manifest["profile"].items():
        payload = record["content"].encode("utf-8")
        assert len(payload) == record["bytes"]
        assert hashlib.sha256(payload).hexdigest() == record["sha256"]
    pre_epoch_fixture = manifest["timestamps"]["pre_epoch"]
    pre_epoch_bytes = pre_epoch_fixture["content"].encode("utf-8")
    assert len(pre_epoch_bytes) == pre_epoch_fixture["bytes"]
    assert hashlib.sha256(pre_epoch_bytes).hexdigest() == \
        pre_epoch_fixture["sha256"]
    metadata_bytes = manifest["metadata"]["file_content"].encode("utf-8")
    assert hashlib.sha256(metadata_bytes).hexdigest() == \
        manifest["metadata"]["sha256"]

    with contextlib.nullcontext(str(RUN_DIR)) as temp:
        root = Path(temp)
        linux = Path(args.linux_boltpack).resolve()
        mode_test = root / "test-posix-restore-mode"
        recorded_run(shlex.split(args.cc) + [
            "-ansi", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-function",
            "-I" + str(ROOT / "src/core"),
            "-I" + str(ROOT / "src/frontend/posix"),
            str(ROOT / "test/unit/test_posix_restore_mode.c"),
            "-o", str(mode_test)], cwd=ROOT, check=True, capture_output=True)
        run([str(mode_test)], root)

        binary = root / "boltpack-macosx-host"
        check_apple_branches_syntax(args.cc)
        build_host_cli(args.cc, binary)

        # Profile create, append, test, and delete all operate on staged files.
        (root / "first.txt").write_bytes(
            manifest["profile"]["first.txt"]["content"].encode("utf-8"))
        (root / "second.txt").write_bytes(
            manifest["profile"]["second.txt"]["content"].encode("utf-8"))
        run([str(binary), "c", "--interchange-profile-1", "profile.bolt",
             "first.txt"], root)
        run([str(binary), "a", "--interchange-profile-1", "profile.bolt",
             "second.txt"], root)
        run([str(binary), "t", "profile.bolt"], root)
        run([str(binary), "d", "profile.bolt", "first.txt"], root)
        run([str(binary), "t", "profile.bolt"], root)
        listing = run([str(binary), "l", "profile.bolt"], root).stdout
        assert b"second.txt" in listing and b"first.txt" not in listing

        # The opt-in modern macOS writer profile preflights the complete
        # resulting archive. The host shim covers ASCII case aliases; native
        # CoreFoundation Unicode equivalence still requires macOS runtime.
        (root / "CaseName.txt").write_bytes(b"profile name collision test\n")
        (root / "casename.txt").write_bytes(b"second spelling\n")
        target_archive = root / "macos-target.bolt"
        run([str(binary), "c", "--target-profile=macos-modern-v1",
             str(target_archive), "CaseName.txt"], root)
        original_target_archive = target_archive.read_bytes()
        run([str(binary), "a", "--target-profile=macos-modern-v1",
             str(target_archive), "casename.txt"], root, expected=1)
        assert target_archive.read_bytes() == original_target_archive, \
            "failed macOS target-profile append modified the existing archive"
        run([str(binary), "x", "--target-profile=macos-modern-v1",
             str(target_archive)], root, expected=1)
        run([str(binary), "c", "--target-profile=unknown",
             "bad-profile.bolt", "CaseName.txt"], root, expected=1)
        run([str(binary), "c", "--target-profile=macos-modern-v1",
             "--target-profile=macos-classic-hfs-v1",
             "mixed-profile.bolt", "CaseName.txt"], root, expected=1)

        # Classic HFS archives retain UTF-8 names but must project exactly to
        # MacRoman and obey the older filesystem's filename and type limits.
        classic_name = "classic-é.txt"
        (root / classic_name).write_bytes(b"MacRoman profile payload\n")
        classic_archive = root / "classic-hfs.bolt"
        run([str(binary), "c", "--target-profile=macos-classic-hfs-v1",
             str(classic_archive), classic_name], root)
        assert archive_entries(classic_archive) == [(classic_name, 0)], \
            "Classic HFS archive should contain only the data entry"
        run([str(binary), "t", str(classic_archive)], root)
        classic_before = classic_archive.read_bytes()
        collision_name = "CLASSIC-É.TXT"
        (root / collision_name).write_bytes(b"case collision\n")
        run([str(binary), "a", "--target-profile=macos-classic-hfs-v1",
             str(classic_archive), collision_name], root, expected=1)
        assert classic_archive.read_bytes() == classic_before, \
            "failed Classic HFS profile append changed the archive"
        (root / "not-macroman-漢.txt").write_bytes(b"unrepresentable\n")
        run([str(binary), "c", "--target-profile=macos-classic-hfs-v1",
             "classic-unrepresentable.bolt", "not-macroman-漢.txt"],
            root, expected=1)
        (root / ("x" * 32)).write_bytes(b"too long for HFS")
        run([str(binary), "c", "--target-profile=macos-classic-hfs-v1",
             "classic-overlong.bolt", "x" * 32], root, expected=1)
        (root / "colon:name").write_bytes(b"forbidden HFS colon\n")
        run([str(binary), "c", "--target-profile=macos-classic-hfs-v1",
             "classic-colon.bolt", "colon:name"], root, expected=1)
        long_tree = root / "longtree"
        long_tree.mkdir()
        current = long_tree
        for _ in range(8):
            current = current / ("d" * 31)
            current.mkdir()
        (current / "leaf").write_bytes(b"overlong HFS path\n")
        run([str(binary), "c", "--target-profile=macos-classic-hfs-v1",
             "classic-overlong-path.bolt", "longtree"], root, expected=1)
        run([str(binary), "c", "--target-profile=macos-classic-hfs-v1",
             "-Z", "classic-bad-codec.bolt", classic_name], root,
            expected=1)
        old_mac_archive = root / "macos-metadata.bolt"
        run([str(binary), "c", str(old_mac_archive), classic_name], root)
        old_mac_before = old_mac_archive.read_bytes()
        run([str(binary), "a", "--target-profile=macos-classic-hfs-v1",
             str(old_mac_archive), "second.txt"], root, expected=1)
        assert old_mac_archive.read_bytes() == old_mac_before, \
            "Classic HFS profile accepted an existing archive with metadata"

        # The macOS CLI can also prepare strict Atari ST archives. GEMDOS
        # checks happen after native Mac mode metadata is omitted.
        atari_dir = root / "ATARISRC"
        atari_dir.mkdir()
        (atari_dir / "NORMAL.TXT").write_bytes(b"Atari profile payload\n")
        atari_archive = root / "atari-profile.bolt"
        run([str(binary), "c", "--target-profile=atari-st",
             str(atari_archive), atari_dir.name], root)
        assert archive_entries(atari_archive) == [
            ("ATARISRC/", 4), ("ATARISRC/NORMAL.TXT", 0)], \
            "Atari profile should omit macOS mode metadata"
        run([str(binary), "t", str(atari_archive)], root)
        atari_before = atari_archive.read_bytes()
        (atari_dir / "normal.txt").write_bytes(b"GEMDOS case collision\n")
        run([str(binary), "a", "--target-profile=atari-st",
             str(atari_archive), "ATARISRC/normal.txt"], root, expected=1)
        assert atari_archive.read_bytes() == atari_before, \
            "failed Atari projected-collision append changed the archive"
        (atari_dir / "öong.txt").write_bytes(b"high-bit GEMDOS name\n")
        run([str(binary), "c", "--target-profile=atari-st",
             "atari-high-bit.bolt", "ATARISRC/öong.txt"], root, expected=1)
        run([str(binary), "c", "--target-profile=atari-st", "-Z",
             "atari-bad-codec.bolt", "ATARISRC/NORMAL.TXT"], root,
            expected=1)
        atari_added = atari_dir / "ADDED.TXT"
        atari_added.write_bytes(b"Atari append\n")
        run([str(binary), "a", "--target-profile=atari-st",
             str(atari_archive), "ATARISRC/ADDED.TXT"], root)
        run([str(binary), "t", str(atari_archive)], root)

        # AmigaOS 68k names use exact Latin-1 projection and 30-byte
        # components. Host-only Unix mode metadata must not leak into output.
        amiga_dir = root / "AMIGASRC"
        amiga_dir.mkdir()
        amiga_name = "café.txt"
        (amiga_dir / amiga_name).write_bytes(b"Amiga Latin-1 profile\n")
        amiga_archive = root / "amiga-profile.bolt"
        run([str(binary), "c", "--target-profile=amigaos-68k-latin1",
             str(amiga_archive), amiga_dir.name], root)
        assert archive_entries(amiga_archive) == [
            ("AMIGASRC/", 4), ("AMIGASRC/" + amiga_name, 0)], \
            "AmigaOS profile should omit macOS mode metadata"
        run([str(binary), "t", str(amiga_archive)], root)
        amiga_added = amiga_dir / "added.txt"
        amiga_added.write_bytes(b"Amiga profile append\n")
        run([str(binary), "a", "--target-profile=amigaos-68k-latin1",
             str(amiga_archive), "AMIGASRC/added.txt"], root)
        run([str(binary), "t", str(amiga_archive)], root)
        amiga_before = amiga_archive.read_bytes()
        (amiga_dir / "CAFÉ.TXT").write_bytes(b"case collision\n")
        run([str(binary), "a", "--target-profile=amigaos-68k-latin1",
             str(amiga_archive), "AMIGASRC/CAFÉ.TXT"], root, expected=1)
        assert amiga_archive.read_bytes() == amiga_before, \
            "failed AmigaOS profile append modified the archive"
        (amiga_dir / "unmappable-漢.txt").write_bytes(b"unmappable\n")
        run([str(binary), "c", "--target-profile=amigaos-68k-latin1",
             "amiga-unmappable.bolt", "AMIGASRC/unmappable-漢.txt"],
            root, expected=1)
        (amiga_dir / ("x" * 31)).write_bytes(b"component too long\n")
        run([str(binary), "c", "--target-profile=amigaos-68k-latin1",
             "amiga-overlong.bolt", "AMIGASRC/" + "x" * 31],
            root, expected=1)
        (amiga_dir / "bad:name.txt").write_bytes(b"forbidden colon\n")
        run([str(binary), "c", "--target-profile=amigaos-68k-latin1",
             "amiga-colon.bolt", "AMIGASRC/bad:name.txt"],
            root, expected=1)
        run([str(binary), "c", "-Z",
             "--target-profile=amigaos-68k-latin1",
             "amiga-bad-codec.bolt", "AMIGASRC/" + amiga_name],
            root, expected=1)
        old_amiga_archive = root / "amiga-existing-mac-metadata.bolt"
        run([str(binary), "c", str(old_amiga_archive),
             "AMIGASRC/" + amiga_name], root)
        old_amiga_before = old_amiga_archive.read_bytes()
        run([str(binary), "a", "--target-profile=amigaos-68k-latin1",
             str(old_amiga_archive), "first.txt"], root, expected=1)
        assert old_amiga_archive.read_bytes() == old_amiga_before, \
            "AmigaOS profile accepted an archive with foreign macOS metadata"

        # DOS/Win16/OS/2 profiles use exact CP437 or CP850 projection and the
        # shared conservative 8.3 and Interchange Profile 1 rules.
        dos850_dir = root / "DOSSRC"
        dos850_dir.mkdir()
        (dos850_dir / "café.txt").write_bytes(b"CP850 profile\n")
        (dos850_dir / "subdir").mkdir()
        (dos850_dir / "subdir" / "DATA.BIN").write_bytes(b"nested 8.3\n")
        dos850_archive = root / "dos850-profile.bolt"
        run([str(binary), "c", "--target-profile=dos-cp850-83",
             str(dos850_archive), dos850_dir.name], root)
        assert archive_entries(dos850_archive) == [
            ("DOSSRC/", 4), ("DOSSRC/café.txt", 0),
            ("DOSSRC/subdir/", 4), ("DOSSRC/subdir/DATA.BIN", 0)], \
            "DOS CP850 profile should retain paths and omit macOS mode metadata"
        run([str(binary), "t", str(dos850_archive)], root)
        dos_added = dos850_dir / "ADDED.TXT"
        dos_added.write_bytes(b"DOS profile append\n")
        run([str(binary), "a", "--target-profile=dos-cp850-83",
             str(dos850_archive), "DOSSRC/ADDED.TXT"], root)
        run([str(binary), "t", str(dos850_archive)], root)

        dos437_dir = root / "D437SRC"
        dos437_dir.mkdir()
        (dos437_dir / "über.txt").write_bytes(b"CP437 profile\n")
        dos_profiles = (
            ("dos-cp850-83", dos850_dir),
            ("dos-cp437-83", dos437_dir),
            ("dos16-cp850-mini", dos850_dir),
            ("win16-cp437-mini", dos437_dir),
            ("os2-16-cp850-mini", dos850_dir),
            ("os2-fat-cp850", dos850_dir),
        )
        for profile_name, source_dir in dos_profiles:
            archive = root / (profile_name + ".bolt")
            run([str(binary), "c", f"--target-profile={profile_name}",
                 str(archive), source_dir.name], root)
            run([str(binary), "t", str(archive)], root)
            run([str(linux), "t", str(archive)], root)

        lfn_name = "Long Filename Ø With Spaces.txt"
        (root / lfn_name).write_bytes(b"DOS LFN profile payload\n")
        lfn_archive = root / "dos-cp850-lfn.bolt"
        run([str(binary), "c", "--target-profile=dos-cp850-lfn-v1",
             str(lfn_archive), lfn_name], root)
        assert archive_entries(lfn_archive) == [(lfn_name, 0)], \
            "DOS LFN profile should preserve the valid CP850 name and omit Unix metadata"
        run([str(binary), "t", str(lfn_archive)], root)
        run([str(linux), "t", str(lfn_archive)], root)
        lfn_before = lfn_archive.read_bytes()
        alias_name = "long filename ø with spaces.TXT"
        (root / alias_name).write_bytes(b"case alias\n")
        run([str(binary), "a", "--target-profile=dos-cp850-lfn-v1",
             str(lfn_archive), alias_name], root, expected=1)
        assert lfn_archive.read_bytes() == lfn_before, \
            "DOS LFN profile accepted a CP850 case-fold collision"
        for invalid_name in ("CON.txt", "bad?.txt", "trailing.",
                             "trailing ", "unmappable-漢.txt"):
            (root / invalid_name).write_bytes(b"invalid DOS name\n")
            bad_archive = root / "dos-lfn-invalid.bolt"
            run([str(binary), "c", "--target-profile=dos-cp850-lfn-v1",
                 str(bad_archive), invalid_name], root, expected=1)
            assert not bad_archive.exists(), \
                "DOS LFN profile published an archive with an invalid name"
        run([str(binary), "c", "-F", "--target-profile=dos-cp850-lfn-v1",
             "dos-lfn-bad-codec.bolt", lfn_name], root, expected=1)

        invalid_dos_dir = root / "DOSBAD"
        invalid_dos_dir.mkdir()
        for invalid_name in ("TOOLONG99.TXT", "CON.TXT", "bad,name.txt",
                             "unmappable-漢.txt"):
            (invalid_dos_dir / invalid_name).write_bytes(b"must be refused\n")
            run([str(binary), "c", "--target-profile=dos-cp850-83",
                 "dos-invalid.bolt", "DOSBAD/" + invalid_name],
                root, expected=1)
            assert not (root / "dos-invalid.bolt").exists(), \
                "invalid DOS profile create published an archive"
        (invalid_dos_dir / "bad,name.txt").unlink()
        (invalid_dos_dir / "unmappable-漢.txt").unlink()
        (invalid_dos_dir / "TOOLONG99.TXT").unlink()
        (invalid_dos_dir / "CON.TXT").unlink()
        (invalid_dos_dir / "alias.txt").write_bytes(b"first alias\n")
        (invalid_dos_dir / "ALIAS.TXT").write_bytes(b"second alias\n")
        run([str(binary), "c", "--target-profile=dos-cp850-83",
             "dos-collision.bolt", invalid_dos_dir.name], root, expected=1)
        assert not (root / "dos-collision.bolt").exists(), \
            "DOS profile accepted a case-fold collision"
        run([str(binary), "c", "-Z", "--target-profile=dos-cp850-83",
             "dos-bad-codec.bolt", "DOSSRC/café.txt"], root, expected=1)
        run([str(binary), "c", "-b", "32k",
             "--target-profile=dos-cp850-83", "dos-bad-block.bolt",
             "DOSSRC/café.txt"], root, expected=1)
        old_dos_archive = root / "dos-existing-mac-metadata.bolt"
        run([str(binary), "c", str(old_dos_archive), "DOSSRC/café.txt"], root)
        old_dos_before = old_dos_archive.read_bytes()
        run([str(binary), "a", "--target-profile=dos-cp850-83",
             str(old_dos_archive), "DOSSRC/ADDED.TXT"], root, expected=1)
        assert old_dos_archive.read_bytes() == old_dos_before, \
            "DOS profile accepted an existing archive with foreign macOS metadata"

        # CP/M-86 is a flat namespace; the writer projects lowercase ASCII to
        # uppercase 8.3 names and omits host metadata entirely.
        (root / "mixed.txt").write_bytes(b"CP/M-86 profile payload\n")
        cpm_archive = root / "cpm86-profile.bolt"
        run([str(binary), "c", "--target-profile=cpm86-83",
             str(cpm_archive), "mixed.txt"], root)
        assert archive_entries(cpm_archive) == [("MIXED.TXT", 0)], \
            "CP/M-86 profile should project the filename and omit metadata"
        run([str(binary), "t", str(cpm_archive)], root)
        run([str(linux), "t", str(cpm_archive)], root)
        cpm_before = cpm_archive.read_bytes()
        (root / "MIXED.TXT").write_bytes(b"case alias\n")
        run([str(binary), "a", "--target-profile=cpm86-83",
             str(cpm_archive), "MIXED.TXT"], root, expected=1)
        assert cpm_archive.read_bytes() == cpm_before, \
            "CP/M-86 profile append accepted an uppercase collision"
        (root / "directory").mkdir()
        run([str(binary), "c", "--target-profile=cpm86-83",
             "cpm-directory.bolt", "directory"], root, expected=1)
        run([str(binary), "c", "-F", "--target-profile=cpm86-83",
             "cpm-bad-codec.bolt", "mixed.txt"], root, expected=1)
        old_cpm_archive = root / "cpm-existing-metadata.bolt"
        run([str(binary), "c", str(old_cpm_archive), "mixed.txt"], root)
        old_cpm_before = old_cpm_archive.read_bytes()
        run([str(binary), "a", "--target-profile=cpm86-83",
             str(old_cpm_archive), "mixed.txt"], root, expected=1)
        assert old_cpm_archive.read_bytes() == old_cpm_before, \
            "CP/M-86 profile accepted existing macOS metadata"

        # Sinclair QL flattens archive separators to underscores. The macOS
        # writer must preserve only representable data and directories, omit
        # host mode metadata, and reject projected aliases on append.
        ql_dir = root / "QLSRC"
        (ql_dir / "dir").mkdir(parents=True)
        (ql_dir / "café.txt").write_bytes(b"QL Latin-1 from macOS\n")
        (ql_dir / "dir" / "naïve.bin").write_bytes(b"nested QL payload\n")
        ql_archive = root / "ql-profile.bolt"
        run([str(binary), "c", "--target-profile=sinclair-ql-latin1-v1",
             str(ql_archive), ql_dir.name], root)
        ql_entries = archive_entries(ql_archive)
        assert ql_entries == [
            ("QLSRC/", 4), ("QLSRC/café.txt", 0),
            ("QLSRC/dir/", 4), ("QLSRC/dir/naïve.bin", 0)], \
            "Sinclair QL profile should preserve Latin-1 names and omit metadata"
        run([str(binary), "t", str(ql_archive)], root)
        run([str(linux), "t", str(ql_archive)], root)
        ql_added = ql_dir / "ADDED.txt"
        ql_added.write_bytes(b"QL append\n")
        run([str(binary), "a", "--target-profile=sinclair-ql-latin1-v1",
             str(ql_archive), str(ql_added.relative_to(root))], root)
        run([str(binary), "t", str(ql_archive)], root)
        run([str(linux), "t", str(ql_archive)], root)
        assert all(kind in (0, 4) for _, kind in archive_entries(ql_archive)), \
            "Sinclair QL profile append introduced unsupported metadata"
        alias_parent = ql_dir / "foo"
        alias_parent.mkdir()
        (ql_dir / "foo_bar").write_bytes(b"flat name\n")
        (alias_parent / "bar").write_bytes(b"nested alias\n")
        run([str(binary), "a", "--target-profile=sinclair-ql-latin1-v1",
             str(ql_archive), str((ql_dir / "foo_bar").relative_to(root))], root)
        ql_before_collision = ql_archive.read_bytes()
        run([str(binary), "a", "--target-profile=sinclair-ql-latin1-v1",
             str(ql_archive), str((alias_parent / "bar").relative_to(root))],
            root, expected=1)
        assert ql_archive.read_bytes() == ql_before_collision, \
            "Sinclair QL alias append changed the existing archive"
        ql_case_alias = ql_dir / "CAFÉ.TXT"
        ql_case_alias.write_bytes(b"Latin-1 case alias\n")
        run([str(binary), "a", "--target-profile=sinclair-ql-latin1-v1",
             str(ql_archive), str(ql_case_alias.relative_to(root))],
            root, expected=1)
        assert ql_archive.read_bytes() == ql_before_collision, \
            "Sinclair QL append accepted a Latin-1 case alias"
        run([str(binary), "c", "--target-profile=sinclair-ql-latin1-v1",
             "ql-collision.bolt",
             str((ql_dir / "foo_bar").relative_to(root)),
             str((alias_parent / "bar").relative_to(root))], root, expected=1)
        assert not (root / "ql-collision.bolt").exists(), \
            "Sinclair QL profile accepted a slash-to-underscore collision"
        (root / ("a" * 36)).write_bytes(b"36-byte QL path\n")
        run([str(binary), "c", "--target-profile=sinclair-ql-latin1-v1",
             "ql-36.bolt", "a" * 36], root)
        (root / ("b" * 37)).write_bytes(b"37-byte QL path\n")
        run([str(binary), "c", "--target-profile=sinclair-ql-latin1-v1",
             "ql-37.bolt", "b" * 37], root, expected=1)
        (root / "unmappable-漢.txt").write_bytes(b"unmappable QL name\n")
        run([str(binary), "c", "--target-profile=sinclair-ql-latin1-v1",
             "ql-unmappable.bolt", "unmappable-漢.txt"], root, expected=1)
        (root / "bad:name.txt").write_bytes(b"forbidden QL name\n")
        run([str(binary), "c", "--target-profile=sinclair-ql-latin1-v1",
             "ql-colon.bolt", "bad:name.txt"], root, expected=1)
        ql_link = root / "ql-link.txt"
        ql_link.symlink_to("QLSRC/café.txt")
        run([str(binary), "c", "--target-profile=sinclair-ql-latin1-v1",
             "ql-link.bolt", ql_link.name], root, expected=1)
        for bad_args, label in ((("-Z",), "zstd"),
                                (("-F",), "fluff"),
                                (("-2",), "boltlz"),
                                (("-b", "32k"), "32k blocks")):
            run([str(binary), "c", *bad_args,
                 "--target-profile=sinclair-ql-latin1-v1",
                 f"ql-bad-{label.replace(' ', '-')}.bolt",
                 str((ql_dir / "café.txt").relative_to(root))],
                root, expected=1)
        ql_stored = root / "ql-stored.bolt"
        run([str(binary), "c", "-0",
             "--target-profile=sinclair-ql-latin1-v1",
             str(ql_stored), str((ql_dir / "café.txt").relative_to(root))], root)
        run([str(binary), "t", str(ql_stored)], root)
        run([str(linux), "t", str(ql_stored)], root)
        ql_foreign_archive = root / "ql-existing-metadata.bolt"
        run([str(binary), "c", str(ql_foreign_archive), "mixed.txt"], root)
        ql_foreign_before = ql_foreign_archive.read_bytes()
        run([str(binary), "a", "--target-profile=sinclair-ql-latin1-v1",
             str(ql_foreign_archive), "first.txt"], root, expected=1)
        assert ql_foreign_archive.read_bytes() == ql_foreign_before, \
            "Sinclair QL profile accepted foreign macOS metadata on append"

        # RISC OS FileCore names use Latin-1, dot separators, ten-byte leaves,
        # and a 255-byte relative path. Periods inside leaves alias paths and
        # must be rejected by the macOS writer too.
        riscos_dir = root / "RISCSRC"
        (riscos_dir / "nested").mkdir(parents=True)
        (riscos_dir / "café_txt").write_bytes(b"RISC OS Latin-1 payload\n")
        (riscos_dir / "nested" / "child").write_bytes(b"RISC OS nested payload\n")
        riscos_archive = root / "riscos-profile.bolt"
        run([str(binary), "c", "--target-profile=riscos-latin1-v1",
             str(riscos_archive), riscos_dir.name], root)
        assert archive_entries(riscos_archive) == [
            ("RISCSRC/", 4), ("RISCSRC/nested/", 4),
            ("RISCSRC/nested/child", 0), ("RISCSRC/café_txt", 0)], \
            "RISC OS profile should preserve Latin-1 paths and omit metadata"
        run([str(binary), "t", str(riscos_archive)], root)
        run([str(linux), "t", str(riscos_archive)], root)
        (riscos_dir / "CAFÉ_TXT").write_bytes(b"case alias\n")
        riscos_before = riscos_archive.read_bytes()
        run([str(binary), "a", "--target-profile=riscos-latin1-v1",
             str(riscos_archive), "RISCSRC/CAFÉ_TXT"], root, expected=1)
        assert riscos_archive.read_bytes() == riscos_before, \
            "RISC OS profile append accepted a Latin-1 case alias"
        for bad_name in ("bad.name", "bad#name", "unmappable-漢"):
            path = root / bad_name
            path.write_bytes(b"invalid RISC OS name\n")
            archive = root / (bad_name.replace('.', '-') + ".bolt")
            run([str(binary), "c", "--target-profile=riscos-latin1-v1",
                 archive.name, path.name], root, expected=1)
            assert not archive.exists(), \
                f"RISC OS profile accepted invalid component {bad_name!r}"
        (root / ("a" * 10)).write_bytes(b"ten bytes\n")
        run([str(binary), "c", "--target-profile=riscos-latin1-v1",
             "riscos-component-10.bolt", "a" * 10], root)
        (root / ("b" * 11)).write_bytes(b"eleven bytes\n")
        run([str(binary), "c", "--target-profile=riscos-latin1-v1",
             "riscos-component-11.bolt", "b" * 11], root, expected=1)
        parts255 = ["c" * 10] * 23 + ["cc"]
        path255 = root
        for part in parts255[:-1]:
            path255 = path255 / part
            path255.mkdir(exist_ok=True)
        (path255 / parts255[-1]).write_bytes(b"255-byte path\n")
        rel255 = "/".join(parts255)
        assert len(rel255.encode("latin-1")) == 255
        run([str(binary), "c", "--target-profile=riscos-latin1-v1",
             "riscos-path-255.bolt", rel255], root)
        parts256 = parts255[:-1] + ["ccc"]
        path256 = root
        for part in parts256[:-1]:
            path256 = path256 / part
            path256.mkdir(exist_ok=True)
        (path256 / parts256[-1]).write_bytes(b"256-byte path\n")
        rel256 = "/".join(parts256)
        assert len(rel256.encode("latin-1")) == 256
        run([str(binary), "c", "--target-profile=riscos-latin1-v1",
             "riscos-path-256.bolt", rel256], root, expected=1)
        for bad_args, label in ((("-Z",), "zstd"),
                                (("-F",), "fluff"),
                                (("-2",), "boltlz"),
                                (("-b", "32k"), "32k-blocks")):
            run([str(binary), "c", *bad_args,
                 "--target-profile=riscos-latin1-v1",
                f"riscos-bad-{label}.bolt", "RISCSRC/café_txt"],
                root, expected=1)
        riscos_foreign_archive = root / "riscos-existing-metadata.bolt"
        run([str(binary), "c", str(riscos_foreign_archive), "mixed.txt"], root)
        riscos_foreign_before = riscos_foreign_archive.read_bytes()
        run([str(binary), "a", "--target-profile=riscos-latin1-v1",
             str(riscos_foreign_archive), "first.txt"], root, expected=1)
        assert riscos_foreign_archive.read_bytes() == riscos_foreign_before, \
            "RISC OS profile accepted existing macOS metadata"

        # A pre-epoch POSIX mtime is unknown in format 6, never wrapped.
        pre_epoch = root / "pre-epoch.txt"
        pre_epoch.write_bytes(pre_epoch_bytes)
        os.utime(pre_epoch, (pre_epoch_fixture["mtime"],
                             pre_epoch_fixture["mtime"]))
        pre_epoch_archive = root / "pre-epoch.bolt"
        run([str(binary), "c", "-0", str(pre_epoch_archive),
             pre_epoch.name], root)
        mtime, flags = primary_mtimes(pre_epoch_archive)[pre_epoch.name]
        assert mtime == pre_epoch_fixture["archived_mtime"] and \
            bool(flags & 0x4000) == pre_epoch_fixture["valid_flag"], \
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
        payload = metadata_bytes
        source_file = source / "epoch.txt"
        source_file.write_bytes(payload)
        source_file.chmod(manifest["metadata"]["file_mode"])
        os.utime(source_file, (manifest["timestamps"]["epoch_zero"],
                               manifest["timestamps"]["epoch_zero"]))
        source.chmod(manifest["metadata"]["directory_mode"])
        os.utime(source, (manifest["timestamps"]["epoch_zero"],
                          manifest["timestamps"]["epoch_zero"]))
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
        assert output_file.stat().st_mode & 0o777 == manifest["metadata"]["file_mode"]
        assert int(output_file.stat().st_mtime) == manifest["timestamps"]["epoch_zero"]
        assert output_dir.stat().st_mode & 0o777 == manifest["metadata"]["directory_mode"]
        assert int(output_dir.stat().st_mtime) == manifest["timestamps"]["epoch_zero"]

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
        recorded_run([sys.executable, str(ROOT / "test/check_extract_race.py"),
                      "--boltpack", str(binary)], cwd=ROOT, check=True,
                     capture_output=True)
        recorded_run([sys.executable, str(ROOT / "test/check_symlink_atomic.py"),
                      "--boltpack", str(binary), "--cc", args.cc],
                     cwd=ROOT, check=True, capture_output=True)

    global RUN_STATE
    RUN_STATE = "PASS: macOS POSIX host lifecycle checks passed"
    print("macOS POSIX host lifecycle checks passed")
    print("Evidence: " + str(RUN_DIR / "run.json"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
