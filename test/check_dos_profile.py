#!/usr/bin/env python3
"""Check full DOS CLI Interchange Profile 1 output on a host build."""
import argparse
from pathlib import Path
import tempfile

from check_ql_profile import check_dry_run, entry_properties, require_ok, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True, type=Path)
    parser.add_argument("--linux-boltpack", required=True, type=Path)
    parser.add_argument("--mini-boltpack", type=Path)
    args = parser.parse_args()
    dos = args.boltpack.resolve()
    readers = [dos, args.linux_boltpack.resolve()]
    if args.mini_boltpack is not None:
        readers.append(args.mini_boltpack.resolve())

    help_result = run(dos, cwd=Path.cwd())
    assert b"--interchange-profile-1" in help_result.stderr

    with tempfile.TemporaryDirectory(prefix="bp_dos_profile_") as temp:
        root = Path(temp)
        (root / "first.bin").write_bytes(b"DOS interchange profile\n" * 2048)
        (root / "second.bin").write_bytes(b"DOS profile append\n" * 1024)

        result = run(dos, "c", "--interchange-profile-1", "profile.blt",
                     "first.bin", cwd=root)
        require_ok(result, "DOS profile create")
        assert entry_properties(root / "profile.blt")["first.bin"] == (1, 16384)
        check_dry_run(dos, "profile.blt", root)
        for reader in readers:
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile create integrity with {reader.name}")

        result = run(dos, "a", "--interchange-profile-1", "profile.blt",
                     "second.bin", cwd=root)
        require_ok(result, "DOS profile append")
        props = entry_properties(root / "profile.blt")
        assert props["first.bin"] == (1, 16384)
        assert props["second.bin"] == (1, 16384)
        for reader in readers:
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile append integrity with {reader.name}")

        result = run(dos, "c", "--interchange-profile-1", "-0",
                     "stored.blt", "first.bin", cwd=root)
        require_ok(result, "DOS Stored profile create")
        assert entry_properties(root / "stored.blt")["first.bin"] == (0, 16384)
        result = run(dos, "a", "--interchange-profile-1", "-0",
                     "stored.blt", "second.bin", cwd=root)
        require_ok(result, "DOS Stored profile append")
        stored_entries = entry_properties(root / "stored.blt")
        assert stored_entries["first.bin"] == (0, 16384)
        assert stored_entries["second.bin"] == (0, 16384)
        for reader in readers:
            require_ok(run(reader, "t", "stored.blt", cwd=root),
                       f"Stored profile create/append integrity with {reader.name}")

        for extra_options in (("-F",), ("-b", "32k")):
            result = run(dos, "c", "--interchange-profile-1", *extra_options,
                         "rejected.blt", "first.bin", cwd=root)
            assert result.returncode != 0
            assert not (root / "rejected.blt").exists()

        result = run(dos, "c", "-F", "extended.blt", "first.bin", cwd=root)
        require_ok(result, "DOS extended-codec archive setup")
        archive = root / "extended.blt"
        before = archive.read_bytes()
        result = run(dos, "a", "--interchange-profile-1", "extended.blt",
                     "second.bin", cwd=root)
        assert result.returncode != 0
        assert archive.read_bytes() == before
        assert not list(root.glob("BP*.TMP"))
        assert not list(root.glob("BK*.BAK"))

    print("DOS Interchange Profile 1 host interoperability checks passed")


if __name__ == "__main__":
    main()
