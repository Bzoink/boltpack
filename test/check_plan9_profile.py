#!/usr/bin/env python3
"""Check Plan 9 CLI Interchange Profile 1 output with host libc shims."""
import hashlib
import json
import pathlib
import stat
import subprocess
import sys
import tempfile

from check_ql_profile import check_dry_run, entry_properties, require_ok, run


def main():
    plan9, full, mini, collision_fixture = (
        pathlib.Path(arg).resolve() for arg in sys.argv[1:5]
    )
    manifest_path = pathlib.Path(sys.argv[5]).resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    profile_files = manifest["profile"]["files"]

    def pinned_bytes(record):
        data = record["content"].encode("utf-8") * record["repetitions"]
        assert len(data) == record["size"]
        assert hashlib.sha256(data).hexdigest() == record["sha256"]
        return data

    with tempfile.TemporaryDirectory(prefix="bp_plan9_profile_") as temp:
        root = pathlib.Path(temp)
        first_bytes = pinned_bytes(profile_files["first.bin"])
        second_bytes = pinned_bytes(profile_files["second.bin"])
        (root / "first.bin").write_bytes(first_bytes)
        (root / "second.bin").write_bytes(second_bytes)

        result = run(plan9, "c", "--interchange-profile-1", "-qq",
                     "profile.blt", "first.bin", cwd=root)
        require_ok(result, "Plan 9 profile create")
        assert entry_properties(root / "profile.blt")["first.bin"] == (
            1, manifest["profile"]["block_size"]
        )
        check_dry_run(plan9, "profile.blt", root)
        for reader in (plan9, full, mini):
            require_ok(run(reader, "t", "-qq", "profile.blt", cwd=root),
                       f"profile create integrity with {reader.name}")
        extracted = root / "profile-output"
        require_ok(run(plan9, "x", "-qq", "-o", str(extracted),
                       "profile.blt", cwd=root), "Plan 9 profile extraction")
        assert (extracted / "first.bin").read_bytes() == first_bytes

        result = run(plan9, "a", "--interchange-profile-1", "-qq",
                     "profile.blt", "second.bin", cwd=root)
        require_ok(result, "Plan 9 profile append")
        props = entry_properties(root / "profile.blt")
        assert props["first.bin"] == (1, manifest["profile"]["block_size"])
        assert props["second.bin"] == (1, manifest["profile"]["block_size"])
        for reader in (plan9, full, mini):
            require_ok(run(reader, "t", "-qq", "profile.blt", cwd=root),
                       f"profile append integrity with {reader.name}")
        appended_output = root / "appended-output"
        require_ok(run(plan9, "x", "-qq", "-o", str(appended_output),
                       "profile.blt", cwd=root), "Plan 9 appended archive extraction")
        assert (appended_output / "first.bin").read_bytes() == first_bytes
        assert (appended_output / "second.bin").read_bytes() == second_bytes

        result = run(plan9, "c", "--interchange-profile-1", "-0", "-qq",
                     "stored.blt", "first.bin", cwd=root)
        require_ok(result, "Plan 9 Stored profile create")
        assert entry_properties(root / "stored.blt")["first.bin"] == (
            0, manifest["profile"]["block_size"]
        )
        for reader in (plan9, full, mini):
            require_ok(run(reader, "t", "-qq", "stored.blt", cwd=root),
                       f"Stored profile integrity with {reader.name}")

        result = run(plan9, "c", "--interchange-profile-1", "-F", "-qq",
                     "rejected.blt", "first.bin", cwd=root)
        assert result.returncode != 0 and not (root / "rejected.blt").exists()

        for collision in manifest["collision_refusals"]:
            reverse = "0" if collision["file_entry_first"] else "1"
            archive = root / f"collision-{reverse}.blt"
            subprocess.run([str(collision_fixture), str(archive), reverse],
                           check=True, cwd=root)
            output = root / f"collision-output-{reverse}"
            result = run(plan9, "x", "-qq", "-o", str(output), str(archive),
                         cwd=root)
            assert result.returncode != 0, "Plan 9 extracted a path collision"
            assert not output.exists(), "preflight created output before refusal"

        metadata_archive = root / "directory-metadata.blt"
        subprocess.run([str(collision_fixture), str(metadata_archive), "metadata"],
                       check=True, cwd=root)
        metadata_output = root / "directory-metadata-output"
        result = run(plan9, "x", "-qq", "-o", str(metadata_output),
                     str(metadata_archive), cwd=root)
        require_ok(result, "Plan 9 nested directory metadata extraction")
        locked = metadata_output / "locked"
        child = locked / "child.txt"
        metadata = manifest["directory_metadata"]
        file_metadata = metadata["file"]
        child_bytes = file_metadata["content"].encode("utf-8")
        assert hashlib.sha256(child_bytes).hexdigest() == file_metadata["sha256"]
        assert child.read_bytes() == child_bytes
        assert stat.S_IMODE(locked.stat().st_mode) == int(metadata["mode"], 8)
        assert stat.S_IMODE(child.stat().st_mode) == int(file_metadata["mode"], 8)
        assert int(locked.stat().st_mtime) == metadata["mtime_epoch"]
        assert int(child.stat().st_mtime) == file_metadata["mtime_epoch"]

    print("Plan 9 profile, extraction-preflight, and metadata host checks passed")


if __name__ == "__main__":
    main()
