#!/usr/bin/env python3
"""Check Linux mini archive metadata and no-clobber behavior."""
import os
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
        os.chmod(file, 0o640)
        os.utime(file, (0, 0))
        os.chmod(child, 0o750)
        os.utime(child, (1234567890, 1234567890))
        os.chmod(source, 0o751)
        os.utime(source, (1234567892, 1234567892))
        result = run(MINI, "c", "mini.blt", "src", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        result = run(MINI, "x", "mini.blt", "out", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        restored = root / "out/src"
        target = restored / "child/file.txt"
        assert target.read_bytes() == b"mini metadata\n"
        assert stat.S_IMODE(target.stat().st_mode) == 0o640
        assert int(target.stat().st_mtime) == 0
        assert stat.S_IMODE((restored / "child").stat().st_mode) == 0o750
        assert int((restored / "child").stat().st_mtime) == 1234567890
        assert stat.S_IMODE(restored.stat().st_mode) == 0o751
        assert int(restored.stat().st_mtime) == 1234567892
        target.write_bytes(b"existing")
        result = run(MINI, "x", "mini.blt", "out", cwd=root)
        assert result.returncode != 0
        assert target.read_bytes() == b"existing"
        (root / "unsafe").mkdir()
        (root / "outside").mkdir()
        (root / "unsafe/src").symlink_to(root / "outside", target_is_directory=True)
        result = run(MINI, "x", "mini.blt", "unsafe", cwd=root)
        assert result.returncode != 0
        assert not (root / "outside/child/file.txt").exists()
        result = run(FULL, "x", "-o", "full", "mini.blt", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        assert (root / "full/src/child/file.txt").read_bytes() == b"mini metadata\n"

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
        result = run(MINI, "x", conflict_archive.name, existing_target.name, cwd=root)
        assert result.returncode != 0, "file/directory conflict was accepted"
        assert sentinel.read_bytes() == b"existing tree remains untouched"
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
        result = run(FULL, "x", "-o", full_existing.name,
                     conflict_archive.name, cwd=root)
        assert result.returncode != 0, "full Linux CLI accepted a path conflict"
        assert full_sentinel.read_bytes() == b"full extractor preserves this tree"
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
        os.utime(old, (-86400, -86400))
        result = run(MINI, "c", "old.blt", "old.txt", cwd=root)
        assert result.returncode != 0 and b"pre-1970" in result.stderr
        result = run(FULL, "t", "old.blt", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        print("Linux mini metadata and interoperability checks passed")


if __name__ == "__main__":
    main()
