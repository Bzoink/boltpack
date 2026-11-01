#!/usr/bin/env python3
"""Check Plan 9 CLI Interchange Profile 1 output with host libc shims."""
import hashlib
import json
import os
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
    metadata_checker = pathlib.Path(sys.argv[6]).resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    profile_files = manifest["profile"]["files"]
    extraction_expected = manifest["extraction"]
    assert extraction_expected["real"] == "new-private-output-root"
    assert extraction_expected["dry_run"] == "succeed-without-output"
    refusal_text = extraction_expected["refusal_message"].encode("ascii")

    def check_extract(archive, output, cwd, expected):
        result = run(plan9, "x", "-qq", "-o", str(output), str(archive), cwd=cwd)
        require_ok(result, "Plan 9 extraction into a new output root")
        assert output.stat().st_mode & 0o077 == 0, \
            "Plan 9 extraction root is accessible to group/other users"
        for name, contents in expected.items():
            assert (output / name).read_bytes() == contents, \
                f"Plan 9 extraction changed bytes for {name}"
        dry_run = run(plan9, "x", "-n", "-qq", "-o", str(output),
                      str(archive), cwd=cwd)
        require_ok(dry_run, "Plan 9 extraction dry-run")
        unused_output = output.with_name(output.name + "-dry-run")
        dry_run = run(plan9, "x", "-n", "-qq", "-o", str(unused_output),
                      str(archive), cwd=cwd)
        require_ok(dry_run, "Plan 9 extraction dry-run with unused root")
        assert not unused_output.exists(), "Plan 9 dry-run created its output root"
        default_output = run(plan9, "x", "-qq", str(archive), cwd=cwd)
        assert default_output.returncode != 0
        assert refusal_text in default_output.stderr
        existing = output / next(iter(expected))
        old_bytes = existing.read_bytes()
        existing_output = run(plan9, "x", "-f", "-qq", "-o", str(output),
                              str(archive), cwd=cwd)
        assert existing_output.returncode != 0
        assert b"output directory must not already exist" in existing_output.stderr
        assert existing.read_bytes() == old_bytes, \
            "Plan 9 extraction modified an existing output tree"

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
        check_extract("profile.blt", extracted, root,
                      {"first.bin": first_bytes})

        # Linux's Plan 9 target profile keeps UTF-8 byte names and the Plan 9
        # reader's supported codecs/entry types. The reader host shim also
        # extracts into a fresh private output root.
        (root / "A").write_bytes(b"case sensitive upper\n")
        (root / "a").write_bytes(b"case sensitive lower\n")
        (root / "folder").mkdir()
        (root / "folder" / "nested.bin").write_bytes(b"nested Plan 9 file\n")
        result = run(full, "create", "--target-profile=plan9-utf8-v1", "-1",
                     "plan9-target.blt", "first.bin", "A", "a", "folder",
                     cwd=root)
        require_ok(result, "Linux Plan 9 target profile create")
        require_ok(run(plan9, "t", "-qq", "plan9-target.blt", cwd=root),
                   "Plan 9 target profile BoltLite integrity")
        require_ok(run(metadata_checker, "plan9-target.blt", "5",
                       cwd=root), "Plan 9 target mode metadata")
        check_dry_run(plan9, "plan9-target.blt", root)
        check_extract("plan9-target.blt", root / "plan9-target-output", root,
                      {"first.bin": first_bytes,
                       "A": b"case sensitive upper\n",
                       "a": b"case sensitive lower\n",
                       "folder/nested.bin": b"nested Plan 9 file\n"})

        result = run(full, "add", "--target-profile=plan9-utf8-v1", "-F",
                     "plan9-target.blt", "second.bin", cwd=root)
        require_ok(result, "Linux Plan 9 target profile append")
        require_ok(run(plan9, "t", "-qq", "plan9-target.blt", cwd=root),
                   "Plan 9 target profile Fluff integrity")
        require_ok(run(metadata_checker, "plan9-target.blt", "6",
                       cwd=root), "Plan 9 appended mode metadata")

        unprofiled = root / "unprofiled.blt"
        require_ok(run(full, "create", "-1", str(unprofiled), "first.bin",
                       cwd=root), "unprofiled archive create")
        unprofiled_bytes = unprofiled.read_bytes()
        result = run(full, "add", "--target-profile=plan9-utf8-v1",
                     str(unprofiled), "second.bin", cwd=root)
        assert result.returncode != 0 and unprofiled.read_bytes() == unprofiled_bytes, \
            "Plan 9 target append accepted existing data without native mode metadata"

        result = run(full, "create", "--target-profile=plan9-utf8-v1", "-Z",
                     "unsupported-zstd.blt", "first.bin", cwd=root)
        assert result.returncode != 0 and not (root / "unsupported-zstd.blt").exists(), \
            "Plan 9 target profile accepted Zstandard"

        for invalid_name in ("control-\x01", "c1-\u0080"):
            bad_source = root / invalid_name
            bad_source.write_bytes(b"must be rejected\n")
            bad_archive = root / ("bad-" + str(len(list(root.iterdir()))) + ".blt")
            result = run(full, "create", "--target-profile=plan9-utf8-v1",
                         str(bad_archive), str(bad_source.name), cwd=root)
            assert result.returncode != 0 and not bad_archive.exists(), \
                f"Plan 9 target profile accepted a control character in {invalid_name!r}"

        future_source = root / "future-time"
        future_source.write_bytes(b"outside Plan 9 timestamp range\n")
        os.utime(future_source, (4294967297, 4294967297))
        future_archive = root / "future-time.blt"
        result = run(full, "create", "--target-profile=plan9-utf8-v1",
                     str(future_archive), future_source.name, cwd=root)
        assert result.returncode != 0 and not future_archive.exists(), \
            "Plan 9 target profile accepted a timestamp outside its 32-bit range"

        too_deep = root / "too-deep"
        too_deep.mkdir()
        current = too_deep
        for index in range(17):
            current = current / (f"d{index:02d}" + "x" * 27)
            current.mkdir()
        (current / "payload").write_bytes(b"path exceeds Plan 9 client buffer\n")
        too_long_archive = root / "too-long-path.blt"
        result = run(full, "create", "--target-profile=plan9-utf8-v1",
                     str(too_long_archive), "too-deep", cwd=root)
        assert result.returncode != 0 and not too_long_archive.exists(), \
            "Plan 9 target profile accepted a path longer than its frontend buffer"

        (root / "unsafe-link").symlink_to("first.bin")
        link_archive = root / "link.blt"
        result = run(full, "create", "--target-profile=plan9-utf8-v1",
                     str(link_archive), "unsafe-link", cwd=root)
        assert result.returncode != 0 and not link_archive.exists(), \
            "Plan 9 target profile accepted a symlink"

        preserved = root / "existing-output"
        preserved.mkdir()
        existing_file = preserved / "first.bin"
        existing_file.write_bytes(b"preserve this destination\n")
        result = run(plan9, "x", "-f", "-qq", "-o", str(preserved),
                     "profile.blt", cwd=root)
        assert result.returncode != 0
        assert b"output directory must not already exist" in result.stderr
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
        check_extract("profile.blt", appended_output, root,
                      {"first.bin": first_bytes, "second.bin": second_bytes})

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
        check_extract(metadata_archive, metadata_output, root,
                      {"locked/child.txt": b"payload"})
        assert (metadata_output / "locked").stat().st_mode & 0o777 == 0o550
        assert (metadata_output / "locked").stat().st_mtime == 1700000000
        assert (metadata_output / "locked/child.txt").stat().st_mode & 0o777 == 0o640
        assert (metadata_output / "locked/child.txt").stat().st_mtime == 1700000001

    print("Plan 9 profile, anchored extraction, metadata, and dry-run host checks passed")


if __name__ == "__main__":
    main()
