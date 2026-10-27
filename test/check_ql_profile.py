#!/usr/bin/env python3
"""Check Sinclair QL CLI Interchange Profile 1 output on a host build."""
import pathlib
import struct
import subprocess
import sys
import tempfile


def run(binary, *args, cwd):
    command = [str(binary), *args]
    if pathlib.Path(binary).name == "bpmini":
        command.insert(0, "qemu-i386")
    return subprocess.run(command, cwd=cwd, capture_output=True)


def entry_properties(path):
    archive = pathlib.Path(path).read_bytes()
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    count = struct.unpack_from("<I", archive, table_offset + 4)[0]
    record = table_offset + 16
    entries = {}
    for _ in range(count):
        name_length = struct.unpack_from("<H", archive, record)[0]
        name = archive[record + 2:record + 2 + name_length].decode("utf-8")
        fields = record + 2 + name_length
        entry_offset = struct.unpack_from("<Q", archive, fields + 26)[0]
        inline_fields = entry_offset + 4 + name_length
        entry_type = archive[fields + 16]
        if entry_type in (0, 3, 4):
            entries[name] = (
                archive[inline_fields + 20],
                struct.unpack_from("<I", archive, inline_fields + 22)[0],
            )
        record = fields + 43
    return entries


def require_ok(result, label):
    assert result.returncode == 0, (
        f"{label} failed ({result.returncode}): "
        f"{result.stderr.decode(errors='replace')}"
    )


def check_dry_run(binary, archive, cwd):
    output = pathlib.Path(cwd) / (pathlib.Path(binary).stem + "-dry-run-output")
    assert not output.exists()
    result = run(binary, "x", "-n", "-o", str(output), str(archive), cwd=cwd)
    require_ok(result, f"{pathlib.Path(binary).name} dry-run verification")
    assert not output.exists(), f"dry-run created {output}"


def main():
    ql, full, mini = (pathlib.Path(arg).resolve() for arg in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="bp_ql_profile_") as temp:
        root = pathlib.Path(temp)
        (root / "first.bin").write_bytes(b"profile transfer\n" * 4096)
        (root / "second.bin").write_bytes(b"append transfer\n" * 1024)

        result = run(ql, "c", "--interchange-profile-1", "profile.blt",
                     "first.bin", cwd=root)
        require_ok(result, "QL profile create")
        assert entry_properties(root / "profile.blt")["first.bin"] == (1, 16384)
        check_dry_run(ql, "profile.blt", root)
        for reader in (ql, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile create integrity with {reader.name}")

        result = run(ql, "a", "--interchange-profile-1", "profile.blt",
                     "second.bin", cwd=root)
        require_ok(result, "QL profile append")
        props = entry_properties(root / "profile.blt")
        assert props["first.bin"] == (1, 16384)
        assert props["second.bin"] == (1, 16384)
        for reader in (ql, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile append integrity with {reader.name}")

        result = run(ql, "c", "--interchange-profile-1", "-0",
                     "stored.blt", "first.bin", cwd=root)
        require_ok(result, "QL Stored profile create")
        assert entry_properties(root / "stored.blt")["first.bin"] == (0, 16384)
        for reader in (ql, full, mini):
            require_ok(run(reader, "t", "stored.blt", cwd=root),
                       f"Stored profile integrity with {reader.name}")

        result = run(ql, "c", "--interchange-profile-1", "-F",
                     "rejected.blt", "first.bin", cwd=root)
        assert result.returncode != 0 and not (root / "rejected.blt").exists()

    print("QL Interchange Profile 1 host interoperability checks passed")


if __name__ == "__main__":
    main()
