#!/usr/bin/env python3
"""Check Plan 9 CLI Interchange Profile 1 output with host libc shims."""
import hashlib
import json
import pathlib
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
    extraction_expected = manifest["extraction"]
    assert extraction_expected["real"] == "refuse-before-output"
    assert extraction_expected["dry_run"] == "succeed-without-output"
    refusal_text = extraction_expected["refusal_message"].encode("ascii")

    def check_refused_extract(archive, output, cwd, existing=None):
        result = run(plan9, "x", "-qq", "-o", str(output), str(archive), cwd=cwd)
        assert result.returncode != 0, "Plan 9 unexpectedly performed unsafe extraction"
        assert refusal_text in result.stderr
        assert not output.exists(), "refused extraction created its output root"
        if existing is not None:
            assert existing.read_bytes() == b"preserve this destination\n"
        dry_run = run(plan9, "x", "-n", "-qq", "-o", str(output),
                      str(archive), cwd=cwd)
        require_ok(dry_run, "Plan 9 extraction dry-run")
        assert not output.exists(), "Plan 9 dry-run created its output root"
        default_output = run(plan9, "x", "-qq", str(archive), cwd=cwd)
        assert default_output.returncode != 0
        assert refusal_text in default_output.stderr

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
        check_refused_extract("profile.blt", extracted, root)

        preserved = root / "existing-output"
        preserved.mkdir()
        existing_file = preserved / "first.bin"
        existing_file.write_bytes(b"preserve this destination\n")
        result = run(plan9, "x", "-f", "-qq", "-o", str(preserved),
                     "profile.blt", cwd=root)
        assert result.returncode != 0 and refusal_text in result.stderr
        assert existing_file.read_bytes() == b"preserve this destination\n"

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
        check_refused_extract("profile.blt", appended_output, root)

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
        for reader in (plan9, full, mini):
            require_ok(run(reader, "t", "-qq", str(metadata_archive), cwd=root),
                       f"metadata fixture integrity with {reader.name}")
        metadata_output = root / "directory-metadata-output"
        check_refused_extract(metadata_archive, metadata_output, root)

    print("Plan 9 profile, extraction-preflight, fail-closed, and dry-run host checks passed")


if __name__ == "__main__":
    main()
