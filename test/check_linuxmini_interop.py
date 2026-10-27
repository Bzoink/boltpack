#!/usr/bin/env python3
"""Check Linux mini archive metadata and no-clobber behavior."""
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MINI = Path(os.environ.get("BP_TEST_LINUXMINI", ROOT / "build/linux32mini/bpmini"))
FULL = ROOT / "build/linux/boltpack"


def run(binary, *args, cwd):
    return subprocess.run([str(binary), *args], cwd=cwd, capture_output=True)


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
