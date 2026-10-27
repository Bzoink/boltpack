#!/usr/bin/env python3
"""Check Plan 9 CLI Interchange Profile 1 output with host libc shims."""
import pathlib
import sys
import tempfile

from check_ql_profile import check_dry_run, entry_properties, require_ok, run


def main():
    plan9, full, mini = (pathlib.Path(arg).resolve() for arg in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="bp_plan9_profile_") as temp:
        root = pathlib.Path(temp)
        (root / "first.bin").write_bytes(b"Plan 9 profile transfer\n" * 2048)
        (root / "second.bin").write_bytes(b"Plan 9 append transfer\n" * 512)

        result = run(plan9, "c", "--interchange-profile-1", "-qq",
                     "profile.blt", "first.bin", cwd=root)
        require_ok(result, "Plan 9 profile create")
        assert entry_properties(root / "profile.blt")["first.bin"] == (1, 16384)
        check_dry_run(plan9, "profile.blt", root)
        for reader in (plan9, full, mini):
            require_ok(run(reader, "t", "-qq", "profile.blt", cwd=root),
                       f"profile create integrity with {reader.name}")

        result = run(plan9, "a", "--interchange-profile-1", "-qq",
                     "profile.blt", "second.bin", cwd=root)
        require_ok(result, "Plan 9 profile append")
        props = entry_properties(root / "profile.blt")
        assert props["first.bin"] == (1, 16384)
        assert props["second.bin"] == (1, 16384)
        for reader in (plan9, full, mini):
            require_ok(run(reader, "t", "-qq", "profile.blt", cwd=root),
                       f"profile append integrity with {reader.name}")

        result = run(plan9, "c", "--interchange-profile-1", "-0", "-qq",
                     "stored.blt", "first.bin", cwd=root)
        require_ok(result, "Plan 9 Stored profile create")
        assert entry_properties(root / "stored.blt")["first.bin"] == (0, 16384)
        for reader in (plan9, full, mini):
            require_ok(run(reader, "t", "-qq", "stored.blt", cwd=root),
                       f"Stored profile integrity with {reader.name}")

        result = run(plan9, "c", "--interchange-profile-1", "-F", "-qq",
                     "rejected.blt", "first.bin", cwd=root)
        assert result.returncode != 0 and not (root / "rejected.blt").exists()

    print("Plan 9 Interchange Profile 1 host interoperability checks passed")


if __name__ == "__main__":
    main()
