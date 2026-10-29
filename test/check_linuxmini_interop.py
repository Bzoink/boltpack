#!/usr/bin/env python3
"""Check Linux mini archive metadata and no-clobber behavior."""
import os
import hashlib
import json
from pathlib import Path
import shlex
import stat
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MINI = Path(os.environ.get("BP_TEST_LINUXMINI", ROOT / "build/linux32mini/bpmini"))
MINI_RUNNER = shlex.split(os.environ.get("BP_TEST_LINUXMINI_RUNNER", ""))
FULL = ROOT / "build/linux/boltpack"
EXPECTED = json.loads((ROOT / "test/reference/interoperability/linux-mini-qemu.json").read_text(encoding="utf-8"))


def check_manifest_file(path, key):
    expected = EXPECTED["files"][key]
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != expected["bytes"] or digest != expected["sha256"]:
        raise AssertionError("independent expected-result mismatch for %s: %d bytes, sha256=%s" %
                             (key, len(data), digest))


def build_path_conflict_helper(directory):
    helper = directory / "make_mini_path_conflict"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sources = [
        ROOT / "test/make_mini_path_conflict.c",
        ROOT / "src/core/mini/archiver.c",
        ROOT / "src/core/mini/compress.c",
        ROOT / "src/core/mini/boltlz_decomp.c",
        ROOT / "src/core/boltlite.c",
        ROOT / "src/core/boltlz.c",
        ROOT / "src/core/bp_codepage.c",
        ROOT / "src/frontend/mini_shared/mini_common.c",
    ]
    command = compiler + [
        "-ansi", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-function",
        "-DBP_MINI", "-DBP_CREATOR_OS=BP_OS_LINUX",
        "-I", str(ROOT / "src/core"),
        "-I", str(ROOT / "src/core/mini"),
        "-I", str(ROOT / "src/frontend/mini_shared"),
        *map(str, sources), "-o", str(helper),
    ]
    subprocess.run(command, check=True, cwd=ROOT, capture_output=True)
    return helper


def make_path_conflict_archive(helper, destination, reverse=False):
    command = [str(helper), str(destination)]
    if reverse:
        command.append("reverse")
    subprocess.run(command, check=True, cwd=ROOT, capture_output=True)


def mini_entry_properties(path):
    archive = path.read_bytes()
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    count = struct.unpack_from("<I", archive, table_offset + 4)[0]
    record = table_offset + 16
    entries = {}
    for _ in range(count):
        name_length = struct.unpack_from("<H", archive, record)[0]
        name = bytes(archive[record + 2:record + 2 + name_length]).decode("utf-8")
        fields = record + 2 + name_length
        entry_offset = struct.unpack_from("<Q", archive, fields + 26)[0]
        inline_fields = entry_offset + 4 + name_length
        method = archive[inline_fields + 20]
        block_size = struct.unpack_from("<I", archive, inline_fields + 22)[0]
        entry_type = archive[fields + 16]
        if entry_type in (0, 3, 4):
            entries[name] = (method, block_size)
        record = fields + 43
    return entries


def run(binary, *args, cwd):
    command = [str(binary), *args]
    if binary == MINI:
        command = [*MINI_RUNNER, *command]
    return subprocess.run(command, cwd=cwd, capture_output=True)


def main():
    with tempfile.TemporaryDirectory(prefix="bp_linuxmini_") as directory:
        root = Path(directory)
        source = root / "src"
        child = source / "child"
        child.mkdir(parents=True)
        file = child / "file.txt"
        file.write_bytes(b"mini metadata\n")
        check_manifest_file(file, "metadata")
        expected_file = EXPECTED["metadata"]["file"]
        expected_child = EXPECTED["metadata"]["child_directory"]
        expected_source = EXPECTED["metadata"]["source_directory"]
        os.chmod(file, int(expected_file["mode_octal"], 8))
        os.utime(file, (expected_file["mtime_unix"], expected_file["mtime_unix"]))
        os.chmod(child, int(expected_child["mode_octal"], 8))
        os.utime(child, (expected_child["mtime_unix"], expected_child["mtime_unix"]))
        os.chmod(source, int(expected_source["mode_octal"], 8))
        os.utime(source, (expected_source["mtime_unix"], expected_source["mtime_unix"]))
        result = run(MINI, "c", "mini.blt", "src", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        result = run(MINI, "x", "mini.blt", "out", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        restored = root / "out/src"
        target = restored / "child/file.txt"
        check_manifest_file(target, "metadata")
        assert stat.S_IMODE(target.stat().st_mode) == int(expected_file["mode_octal"], 8)
        assert int(target.stat().st_mtime) == expected_file["mtime_unix"]
        assert stat.S_IMODE((restored / "child").stat().st_mode) == int(expected_child["mode_octal"], 8)
        assert int((restored / "child").stat().st_mtime) == expected_child["mtime_unix"]
        assert stat.S_IMODE(restored.stat().st_mode) == int(expected_source["mode_octal"], 8)
        assert int(restored.stat().st_mtime) == expected_source["mtime_unix"]
        target.write_bytes(b"existing")
        result = run(MINI, "x", "mini.blt", "out", cwd=root)
        assert result.returncode != 0
        check_manifest_file(target, "overwrite_sentinel")
        (root / "unsafe").mkdir()
        (root / "outside").mkdir()
        (root / "unsafe/src").symlink_to(root / "outside", target_is_directory=True)
        result = run(MINI, "x", "mini.blt", "unsafe", cwd=root)
        assert result.returncode != 0
        assert not (root / "outside/child/file.txt").exists()
        result = run(FULL, "x", "-o", "full", "mini.blt", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        full_restored = root / "full/src"
        full_target = full_restored / "child/file.txt"
        check_manifest_file(full_target, "metadata")
        assert stat.S_IMODE(full_target.stat().st_mode) == int(expected_file["mode_octal"], 8)
        assert int(full_target.stat().st_mtime) == expected_file["mtime_unix"]
        assert stat.S_IMODE((full_restored / "child").stat().st_mode) == int(expected_child["mode_octal"], 8)
        assert int((full_restored / "child").stat().st_mtime) == expected_child["mtime_unix"]
        assert stat.S_IMODE(full_restored.stat().st_mode) == int(expected_source["mode_octal"], 8)
        assert int(full_restored.stat().st_mtime) == expected_source["mtime_unix"]

        # Mini must support many directories beyond the former 1,024-node
        # metadata queue while finalizing every directory after creation.
        manydirs = root / "manydirs"
        manydirs.mkdir()
        for index in range(4097):
            (manydirs / ("d%04d" % index)).mkdir()
        os.utime(manydirs / "d0000", (1600000000, 1600000000))
        os.utime(manydirs, (1600000001, 1600000001))
        result = run(FULL, "c", "-0", "manydirs.blt", "manydirs", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        manydirs_out = root / "manydirs-out"
        result = run(MINI, "x", "manydirs.blt", manydirs_out.name, cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        restored_dirs = manydirs_out / "manydirs"
        assert restored_dirs.is_dir() and len(list(restored_dirs.iterdir())) == 4097, \
            "mini did not extract and finalize all 4,097 child directories"
        assert int(restored_dirs.stat().st_mtime) == 1600000001
        assert int((restored_dirs / "d0000").stat().st_mtime) == 1600000000

        profile_data = root / "profile.bin"
        profile_data.write_bytes(b"A" * 65536)
        check_manifest_file(profile_data, "profile")
        result = run(MINI, "c", "--interchange-profile-1", "mini-profile.blt",
                     "profile.bin", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        profile_entries = mini_entry_properties(root / "mini-profile.blt")
        assert profile_entries["profile.bin"] == (1, 16384), \
            f"mini profile did not select BoltLite/16 KiB: {profile_entries}"
        assert run(MINI, "t", "mini-profile.blt", cwd=root).returncode == 0
        assert run(FULL, "t", "mini-profile.blt", cwd=root).returncode == 0
        mini_profile_full_out = root / "mini-profile-full-out"
        result = run(FULL, "x", "-o", mini_profile_full_out.name,
                     "mini-profile.blt", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        check_manifest_file(mini_profile_full_out / "profile.bin", "profile")

        result = run(FULL, "c", "--interchange-profile-1", "full-profile.blt",
                     "profile.bin", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        assert run(MINI, "t", "full-profile.blt", cwd=root).returncode == 0
        full_profile_mini_out = root / "full-profile-mini-out"
        result = run(MINI, "x", "full-profile.blt", full_profile_mini_out.name,
                     cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        check_manifest_file(full_profile_mini_out / "profile.bin", "profile")

        result = run(MINI, "c", "--interchange-profile-1", "-m0",
                     "mini-profile-stored.blt", "profile.bin", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        stored_entries = mini_entry_properties(root / "mini-profile-stored.blt")
        assert stored_entries["profile.bin"] == (0, 16384)
        assert run(MINI, "t", "mini-profile-stored.blt", cwd=root).returncode == 0
        assert run(FULL, "t", "mini-profile-stored.blt", cwd=root).returncode == 0
        result = run(MINI, "c", "--interchange-profile-1", "-m2",
                     "mini-profile-rejected.blt", "profile.bin", cwd=root)
        assert result.returncode != 0
        assert not (root / "mini-profile-rejected.blt").exists()

        conflict_archive = root / "prefix-conflict.blt"
        helper = build_path_conflict_helper(root)
        make_path_conflict_archive(helper, conflict_archive)
        absent_target = root / "conflict-absent"
        result = run(MINI, "x", conflict_archive.name, absent_target.name, cwd=root)
        assert result.returncode != 0, "file/directory conflict was accepted"
        assert not absent_target.exists(), "preflight created a destination before refusal"
        reverse_archive = root / "prefix-conflict-reverse.blt"
        make_path_conflict_archive(helper, reverse_archive, reverse=True)
        reverse_target = root / "conflict-reverse"
        result = run(MINI, "x", reverse_archive.name, reverse_target.name, cwd=root)
        assert result.returncode != 0, "reversed file/directory conflict was accepted"
        assert not reverse_target.exists(), "reversed preflight created a destination"
        existing_target = root / "conflict-existing"
        existing_target.mkdir()
        sentinel = existing_target / "sentinel"
        sentinel.write_bytes(b"existing tree remains untouched")
        check_manifest_file(sentinel, "mini_conflict_sentinel")
        result = run(MINI, "x", conflict_archive.name, existing_target.name, cwd=root)
        assert result.returncode != 0, "file/directory conflict was accepted"
        check_manifest_file(sentinel, "mini_conflict_sentinel")
        assert sorted(path.relative_to(existing_target).as_posix()
                      for path in existing_target.rglob("*")) == ["sentinel"]
        for source_archive in (conflict_archive, reverse_archive):
            target_name = "full-" + source_archive.stem
            target = root / target_name
            result = run(FULL, "x", "-o", target_name, source_archive.name, cwd=root)
            assert result.returncode != 0, "full Linux CLI accepted a path conflict"
            assert not target.exists(), "full Linux preflight created destination"
        full_existing = root / "full-conflict-existing"
        full_existing.mkdir()
        full_sentinel = full_existing / "sentinel"
        full_sentinel.write_bytes(b"full extractor preserves this tree")
        check_manifest_file(full_sentinel, "full_conflict_sentinel")
        result = run(FULL, "x", "-o", full_existing.name,
                     conflict_archive.name, cwd=root)
        assert result.returncode != 0, "full Linux CLI accepted a path conflict"
        check_manifest_file(full_sentinel, "full_conflict_sentinel")
        assert sorted(path.relative_to(full_existing).as_posix()
                      for path in full_existing.rglob("*")) == ["sentinel"]

        archive = bytearray((root / "mini.blt").read_bytes())
        table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
        count = struct.unpack_from("<I", archive, table_offset + 4)[0]
        record = table_offset + 16
        found = False
        for _ in range(count):
            name_len = struct.unpack_from("<H", archive, record)[0]
            name = bytes(archive[record + 2:record + 2 + name_len])
            fields = record + 2 + name_len
            if name == b"src/child/file.txt":
                entry_offset = struct.unpack_from("<Q", archive, fields + 26)[0]
                header_size = struct.unpack_from("<H", archive, entry_offset)[0]
                archive[entry_offset + header_size + 12] ^= 1
                found = True
                break
            record = fields + 43
        assert found, "file entry not found"
        (root / "corrupt.blt").write_bytes(archive)
        result = run(MINI, "x", "corrupt.blt", "corrupt-out", cwd=root)
        assert result.returncode != 0, "corrupt data was accepted"
        assert not (root / "corrupt-out/src/child/file.txt").exists()
        assert not list((root / "corrupt-out").rglob(".boltpack-*")), "temporary file remains"

        old = root / "old.txt"
        old.write_bytes(b"old timestamp")
        check_manifest_file(old, "old_time_source")
        negative_time = EXPECTED["metadata"]["negative_time_source_mtime_unix"]
        os.utime(old, (negative_time, negative_time))
        result = run(MINI, "c", "old.blt", "old.txt", cwd=root)
        assert result.returncode != 0 and b"pre-1970" in result.stderr
        result = run(FULL, "t", "old.blt", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        print("Linux mini metadata and interoperability checks passed")


if __name__ == "__main__":
    main()
