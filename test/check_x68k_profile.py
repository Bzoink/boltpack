#!/usr/bin/env python3
"""Check X68000 CLI Interchange Profile 1 output with host DOS stubs."""
import pathlib
import sys
import tempfile

from check_ql_profile import check_dry_run, entry_properties, require_ok, run


def main():
    x68k, full, mini = (pathlib.Path(arg).resolve() for arg in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="bp_x68k_profile_") as temp:
        root = pathlib.Path(temp)
        (root / "first.bin").write_bytes(b"X68000 profile transfer\n" * 2048)
        (root / "second.bin").write_bytes(b"X68000 append transfer\n" * 512)

        result = run(x68k, "c", "--interchange-profile-1", "profile.blt",
                     "first.bin", cwd=root)
        require_ok(result, "X68000 profile create")
        assert entry_properties(root / "profile.blt")["first.bin"] == (1, 16384)
        check_dry_run(x68k, "profile.blt", root)
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile create integrity with {reader.name}")

        result = run(x68k, "a", "--interchange-profile-1", "profile.blt",
                     "second.bin", cwd=root)
        require_ok(result, "X68000 profile append")
        props = entry_properties(root / "profile.blt")
        assert props["first.bin"] == (1, 16384)
        assert props["second.bin"] == (1, 16384)
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile append integrity with {reader.name}")

        result = run(x68k, "c", "--interchange-profile-1", "-0",
                     "stored.blt", "first.bin", cwd=root)
        require_ok(result, "X68000 Stored profile create")
        assert entry_properties(root / "stored.blt")["first.bin"] == (0, 16384)
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "stored.blt", cwd=root),
                       f"Stored profile integrity with {reader.name}")

        result = run(x68k, "c", "--interchange-profile-1", "-b", "32k",
                     "rejected.blt", "first.bin", cwd=root)
        assert result.returncode != 0 and not (root / "rejected.blt").exists()

    print("X68000 Interchange Profile 1 host interoperability checks passed")


if __name__ == "__main__":
    main()
