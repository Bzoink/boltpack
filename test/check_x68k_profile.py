#!/usr/bin/env python3
"""Check X68000 CLI Interchange Profile 1 output with host DOS stubs."""
import pathlib
import os
import hashlib
import json
import subprocess
import sys
import tempfile

from check_ql_profile import (check_dry_run, entry_properties, entry_times,
                              require_ok, run)


def main():
    x68k, full, mini, manifest_path, collision_fixture = (
        pathlib.Path(arg).resolve() for arg in sys.argv[1:6]
    )
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    def pinned_bytes(record):
        data = record["content"].encode("utf-8") * record.get("repetitions", 1)
        assert len(data) == record.get("size", len(data))
        assert hashlib.sha256(data).hexdigest() == record["sha256"]
        return data

    profile_files = manifest["profile"]["files"]
    with tempfile.TemporaryDirectory(prefix="bp_x68k_profile_") as temp:
        root = pathlib.Path(temp)
        first_bytes = pinned_bytes(profile_files["first.bin"])
        second_bytes = pinned_bytes(profile_files["second.bin"])
        (root / "first.bin").write_bytes(first_bytes)
        (root / "second.bin").write_bytes(second_bytes)

        # Human68k's DOS clock is local wall time. With an explicit zero
        # offset, an even-second source mtime must round-trip as format-6 UTC.
        timed_source = root / "timed.bin"
        timestamp_fixture = manifest["timestamp"]
        timestamp_bytes = pinned_bytes(timestamp_fixture)
        timed_source.write_bytes(timestamp_bytes)
        expected_time = timestamp_fixture["utc_epoch"]
        os.utime(timed_source, (expected_time, expected_time))
        tz_env = os.environ.copy()
        tz_env["BP_TZ_OFFSET"] = str(
            timestamp_fixture["offset_minutes_east_of_utc"])
        timed_create = subprocess.run(
            [str(x68k), "c", "--interchange-profile-1", "timed.blt", "timed.bin"],
            cwd=root, env=tz_env, capture_output=True)
        require_ok(timed_create, "X68000 timestamped profile create")
        assert entry_times(root / "timed.blt")["timed.bin"] == (
            expected_time, 0x4000)
        timed_output = root / "timed-output"
        timed_extract = subprocess.run(
            [str(x68k), "x", "-o", str(timed_output), "timed.blt"],
            cwd=root, env=tz_env, capture_output=True)
        require_ok(timed_extract, "X68000 timestamped extraction")
        assert int((timed_output / "timed.bin").stat().st_mtime) == expected_time

        unknown_env = os.environ.copy()
        unknown_env.pop("BP_TZ_OFFSET", None)
        unknown_create = subprocess.run(
            [str(x68k), "c", "--interchange-profile-1", "unknown-time.blt",
             "timed.bin"], cwd=root, env=unknown_env, capture_output=True)
        require_ok(unknown_create, "X68000 unknown-time profile create")
        unknown_mtime, unknown_flags = entry_times(
            root / "unknown-time.blt")["timed.bin"]
        assert unknown_mtime == 0 and not (unknown_flags & 0x4000), \
            "X68000 local timestamp without timezone was falsely marked valid"

        partial_output = root / "partial-time-output"
        partial_extract = subprocess.run(
            [str(x68k), "x", "-o", str(partial_output), "timed.blt"],
            cwd=root, env=unknown_env, capture_output=True)
        assert partial_extract.returncode != 0 and \
            b"partial success" in partial_extract.stderr, \
            "X68000 did not report the missing extraction timezone as partial"
        assert (partial_output / "timed.bin").read_bytes() == timed_source.read_bytes()

        for collision_case in manifest["collision_refusals"]:
            case = collision_case["case"]
            archive = root / f"collision-{case}.blt"
            subprocess.run([str(collision_fixture), str(archive), case],
                           check=True, cwd=root)
            output = root / f"collision-output-{case}"
            collision = run(x68k, "x", "-o", str(output), str(archive),
                            cwd=root)
            assert collision.returncode != 0, \
                f"X68000 accepted destination collision fixture {case}"
            assert not output.exists(), \
                f"X68000 created output before refusing collision {case}"

        # Portable FAT flags must survive both file and deferred-directory
        # extraction. The host DOS shim persists the DOS attribute byte in an
        # xattr and models READONLY with host write bits.
        attr_source = root / "attrs"
        attr_source.mkdir()
        fat_fixture = manifest["portable_fat"]
        file_attributes = fat_fixture["file"]
        attr_file = attr_source / "locked.bin"
        attr_bytes = pinned_bytes(file_attributes)
        attr_file.write_bytes(attr_bytes)
        os.setxattr(attr_file, "user.bp_x68k_attr",
                    bytes([file_attributes["value"]]))
        attr_file.chmod(0o444)
        attr_dir = attr_source / "locked-dir"
        attr_dir.mkdir()
        directory_attributes = fat_fixture["directory"]
        os.setxattr(attr_dir, "user.bp_x68k_attr", bytes([
            directory_attributes["native_directory_bit"] |
            directory_attributes["value"]]))
        attr_child = attr_dir / "child.bin"
        child_bytes = directory_attributes["child_content"].encode("utf-8")
        assert hashlib.sha256(child_bytes).hexdigest() == \
            directory_attributes["child_sha256"]
        attr_child.write_bytes(child_bytes)
        attr_archive = root / "attrs.blt"
        attr_create = run(x68k, "c", "--interchange-profile-1",
                          "attrs.blt", "attrs", cwd=root)
        require_ok(attr_create, "X68000 portable FAT metadata create")
        attr_output = root / "attrs-output"
        attr_extract = run(x68k, "x", "-o", str(attr_output),
                           "attrs.blt", cwd=root)
        require_ok(attr_extract, "X68000 portable FAT metadata extraction")
        restored_attr_file = attr_output / "attrs" / "locked.bin"
        restored_attr_dir = attr_output / "attrs" / "locked-dir"
        assert os.getxattr(restored_attr_file, "user.bp_x68k_attr") == \
            bytes([file_attributes["value"]])
        assert restored_attr_file.stat().st_mode & 0o222 == 0
        assert os.getxattr(restored_attr_dir, "user.bp_x68k_attr") == bytes([
            directory_attributes["native_directory_bit"] |
            directory_attributes["value"]])
        assert restored_attr_dir.stat().st_mode & 0o222 == 0
        assert (restored_attr_dir / "child.bin").read_bytes() == \
            attr_child.read_bytes()

        result = run(x68k, "c", "--interchange-profile-1", "profile.blt",
                     "first.bin", cwd=root)
        require_ok(result, "X68000 profile create")
        archive_attributes = 0x22  # portable ARCHIVE + HIDDEN
        os.setxattr(root / "profile.blt", "user.bp_x68k_attr",
                    bytes([archive_attributes]))
        info = run(full, "i", "profile.blt", cwd=root)
        require_ok(info, "X68000 profile identity inspection")
        assert b"Creator OS:     Sharp X68000" in info.stdout
        assert b"Creator CPU:    Motorola 68k" in info.stdout
        assert entry_properties(root / "profile.blt")["first.bin"] == (1, 16384)
        check_dry_run(x68k, "profile.blt", root)
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile create integrity with {reader.name}")

        result = run(x68k, "a", "--interchange-profile-1", "profile.blt",
                     "second.bin", cwd=root)
        require_ok(result, "X68000 profile append")
        assert os.getxattr(root / "profile.blt", "user.bp_x68k_attr") == \
            bytes([archive_attributes]), \
            "X68000 append lost the archive's portable FAT attributes"
        props = entry_properties(root / "profile.blt")
        assert props["first.bin"] == (1, 16384)
        assert props["second.bin"] == (1, 16384)
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile append integrity with {reader.name}")

        # Create, append, and delete operate on same-directory stages. An
        # injected edit failure must leave the original archive byte-for-byte
        # intact and clean up the temporary sibling.
        edit_env = os.environ.copy()
        edit_env["BP_X68K_FAIL_STAGE_EDIT"] = "1"
        for command, arguments in (
                ("c", ("profile.blt", "first.bin")),
                ("a", ("profile.blt", "first.bin")),
                ("d", ("profile.blt", "first.bin"))):
            before = (root / "profile.blt").read_bytes()
            failed_edit = subprocess.run(
                [str(x68k), command, *arguments], cwd=root, env=edit_env,
                capture_output=True)
            assert failed_edit.returncode != 0, \
                f"X68000 {command} unexpectedly passed injected stage failure"
            assert (root / "profile.blt").read_bytes() == before, \
                f"X68000 {command} changed archive after stage failure"
            assert not list(root.glob("BP*.TMP")), \
                f"X68000 {command} left a temporary archive stage"
            assert not list(root.glob("BP*.BAK")), \
                f"X68000 {command} left an archive backup"

        replacement_archive = root / "replacement.blt"
        replacement_archive.write_bytes((root / "profile.blt").read_bytes())
        os.setxattr(replacement_archive, "user.bp_x68k_attr",
                    bytes([archive_attributes]))
        replacement = run(x68k, "c", "--interchange-profile-1",
                          "replacement.blt", "first.bin", cwd=root)
        require_ok(replacement, "X68000 staged create replacement")
        assert os.getxattr(replacement_archive, "user.bp_x68k_attr") == \
            bytes([archive_attributes]), \
            "X68000 create replacement lost portable FAT attributes"
        require_ok(run(full, "t", "replacement.blt", cwd=root),
                   "X68000 create replacement integrity")

        # Failed publication after the old archive is moved to its backup must
        # roll the original archive back into place.
        before_publish = (root / "profile.blt").read_bytes()
        publish_env = os.environ.copy()
        publish_env["BP_X68K_FAIL_STAGE_PUBLISH"] = "1"
        failed_archive_publish = subprocess.run(
            [str(x68k), "a", "--interchange-profile-1", "profile.blt",
             "first.bin"], cwd=root, env=publish_env, capture_output=True)
        assert failed_archive_publish.returncode != 0
        assert (root / "profile.blt").read_bytes() == before_publish, \
            "failed X68000 archive publication did not restore original bytes"
        assert not list(root.glob("BP*.TMP"))
        assert not list(root.glob("BP*.BAK"))

        deleted = run(x68k, "d", "profile.blt", "second.bin", cwd=root)
        require_ok(deleted, "X68000 staged delete")
        after_delete = entry_properties(root / "profile.blt")
        assert "first.bin" in after_delete and "second.bin" not in after_delete
        assert os.getxattr(root / "profile.blt", "user.bp_x68k_attr") == \
            bytes([archive_attributes]), \
            "X68000 delete lost the archive's portable FAT attributes"
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile delete integrity with {reader.name}")

        result = run(x68k, "c", "--interchange-profile-1", "-0",
                     "stored.blt", "first.bin", cwd=root)
        require_ok(result, "X68000 Stored profile create")
        assert entry_properties(root / "stored.blt")["first.bin"] == (0, 16384)
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "stored.blt", cwd=root),
                       f"Stored profile integrity with {reader.name}")

        # A checksum failure must discard the sibling stage and preserve an
        # existing destination even when force replacement was requested.
        stored_bytes = bytearray((root / "stored.blt").read_bytes())
        payload_offset = stored_bytes.find(b"X68000 profile transfer\n")
        assert payload_offset >= 0, "stored payload not found in fixture archive"
        stored_bytes[payload_offset + 8] ^= 0x80
        (root / "corrupt.blt").write_bytes(stored_bytes)
        corrupt_output = root / "corrupt-output"
        (corrupt_output / "first.bin").parent.mkdir(parents=True)
        preserved = corrupt_output / "first.bin"
        preserved.write_bytes(b"existing target survives checksum failure\n")
        result = run(x68k, "x", "-f", "-o", str(corrupt_output),
                     "corrupt.blt", cwd=root)
        assert result.returncode != 0, "X68000 accepted a corrupt payload"
        assert preserved.read_bytes() == b"existing target survives checksum failure\n"
        assert not list(corrupt_output.glob("BP*.TMP")), \
            "failed X68000 extraction left a stage"
        assert not list(corrupt_output.glob("BP*.BAK")), \
            "failed X68000 extraction left a recovery file"
        replacement_before = preserved.read_bytes()
        publish_env = os.environ.copy()
        publish_env["BP_X68K_FAIL_STAGE_PUBLISH"] = "1"
        failed_publish = subprocess.run(
            [str(x68k), "x", "-f", "-o", str(corrupt_output), "stored.blt"],
            cwd=root, env=publish_env, capture_output=True)
        assert failed_publish.returncode != 0
        assert preserved.read_bytes() == replacement_before, \
            "failed X68000 publication did not roll back the destination"
        assert not list(corrupt_output.glob("BP*.TMP"))
        assert not list(corrupt_output.glob("BP*.BAK"))
        skipped = run(x68k, "x", "-o", str(corrupt_output), "stored.blt",
                      cwd=root)
        assert skipped.returncode != 0 and preserved.read_bytes() == \
            b"existing target survives checksum failure\n", \
            "X68000 no-force extraction replaced an existing destination"
        require_ok(run(x68k, "x", "-f", "-o", str(corrupt_output),
                       "stored.blt", cwd=root), "X68000 forced staged replacement")
        assert preserved.read_bytes() == (root / "first.bin").read_bytes()
        assert not list(corrupt_output.glob("BP*.TMP"))
        assert not list(corrupt_output.glob("BP*.BAK"))

        source_tree = root / "source-tree"
        (source_tree / "nested").mkdir(parents=True)
        (source_tree / "empty").mkdir()
        (source_tree / "nested" / "payload.bin").write_bytes(
            b"nested X68000 payload\n" * 2048)
        result = run(x68k, "c", "--interchange-profile-1", "tree.blt",
                     "source-tree", cwd=root)
        require_ok(result, "X68000 nested directory create")
        assert entry_properties(root / "tree.blt")[
            "source-tree/nested/payload.bin"] == (1, 16384)
        listing = run(x68k, "l", "tree.blt", cwd=root)
        require_ok(listing, "X68000 nested directory listing")
        assert b"source-tree/empty/" in listing.stdout
        for reader in (x68k, full, mini):
            require_ok(run(reader, "t", "tree.blt", cwd=root),
                       f"nested directory integrity with {reader.name}")

        # The archive path is valid on its own, but cannot be joined to this
        # output root in Human68k's fixed 256-byte path buffer. Refuse it
        # rather than extracting under a silently truncated destination.
        long_output = "o" * 250
        result = run(x68k, "x", "-o", long_output, "tree.blt", cwd=root)
        assert result.returncode != 0 and b"output path too long" in result.stderr
        assert not (root / long_output).exists(), \
            "X68000 created output root before joined-path preflight"

        # The output root itself must fit the frontend's path buffer. Refuse
        # before creating any truncated prefix of a longer multi-component
        # destination.
        long_root = "/".join(("a" * 110, "b" * 110, "c" * 40))
        assert len(long_root) >= 256
        result = run(x68k, "x", "-o", long_root, "tree.blt", cwd=root)
        assert result.returncode != 0 and b"output path too long" in result.stderr
        assert not (root / ("a" * 110)).exists()

        result = run(x68k, "c", "--interchange-profile-1", "-b", "32k",
                     "rejected.blt", "first.bin", cwd=root)
        assert result.returncode != 0 and not (root / "rejected.blt").exists()

        # Standard JIS X 0208 names round-trip through UTF-8 archive paths.
        # The independent manifest pins a character whose second byte is
        # 0x5c, which path scanners must not interpret as a separator.
        sjis_fixture = manifest["shift_jis"]
        sjis_directory = bytes.fromhex(sjis_fixture["local_name_hex"])
        unicode_directory = sjis_fixture["unicode_name"]
        payload = sjis_fixture["payload"].encode("utf-8")
        assert hashlib.sha256(payload).hexdigest() == \
            sjis_fixture["payload_sha256"]
        os.mkdir(os.fsencode(root) + b"/" + sjis_directory)
        with open(os.fsencode(root) + b"/" + sjis_directory + b"/payload.bin", "wb") as out:
            out.write(payload)
        japanese_archive = root / "japanese.blt"
        created = subprocess.run(
            [os.fsencode(x68k), b"c", b"japanese.blt", sjis_directory],
            cwd=os.fsencode(root), capture_output=True)
        require_ok(created, "X68000 standard Shift-JIS directory create")
        assert f"{unicode_directory}/payload.bin" in entry_properties(japanese_archive)
        host_output = root / "japanese-host-output"
        extracted = run(x68k, "x", "-o", str(host_output), "japanese.blt", cwd=root)
        require_ok(extracted, "X68000 standard Shift-JIS extraction")
        assert open(os.fsencode(host_output) + b"/" + sjis_directory +
                    b"/payload.bin", "rb").read() == payload
        linux_output = root / "japanese-linux-output"
        extracted = run(full, "x", "-o", str(linux_output), "japanese.blt", cwd=root)
        assert extracted.returncode in (0, 1), \
            f"Linux failed to extract the X68000 Shift-JIS archive: {extracted.stderr!r}"
        assert (linux_output / unicode_directory / "payload.bin").read_bytes() == payload

        # Undefined standard Shift-JIS pairs remain a hard refusal. The old
        # archive must survive failed create and append unchanged.
        sjis_directory = bytes.fromhex(sjis_fixture["undefined_pair_hex"])
        os.mkdir(os.fsencode(root) + b"/" + sjis_directory)
        protected_archive = root / "protected.blt"
        protected_bytes = (root / "tree.blt").read_bytes()
        protected_archive.write_bytes(protected_bytes)
        rejected = subprocess.run(
            [os.fsencode(x68k), b"c", b"protected.blt", sjis_directory],
            cwd=os.fsencode(root), capture_output=True)
        assert rejected.returncode != 0
        assert protected_archive.read_bytes() == protected_bytes, \
            "unrepresentable X68000 directory name changed the existing archive"

        descendant_root = os.fsencode(root / "descendant-tree")
        os.mkdir(descendant_root)
        os.mkdir(descendant_root + b"/" + sjis_directory)
        rejected = subprocess.run(
            [os.fsencode(x68k), b"c", b"protected.blt", b"descendant-tree"],
            cwd=os.fsencode(root), capture_output=True)
        assert rejected.returncode != 0, \
            "unrepresentable nested X68000 directory name was accepted"
        assert protected_archive.read_bytes() == protected_bytes, \
            "nested unrepresentable name changed the existing archive"

        rejected = subprocess.run(
            [os.fsencode(x68k), b"a", b"protected.blt", b"descendant-tree"],
            cwd=os.fsencode(root), capture_output=True)
        assert rejected.returncode != 0, \
            "add accepted an unrepresentable nested X68000 directory name"
        assert protected_archive.read_bytes() == protected_bytes, \
            "failed add changed the existing archive"

    print("X68000 Interchange Profile 1 host interoperability checks passed")


if __name__ == "__main__":
    main()
