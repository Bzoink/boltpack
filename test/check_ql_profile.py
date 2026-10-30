#!/usr/bin/env python3
"""Check Sinclair QL CLI Interchange Profile 1 output on a host build."""
import hashlib
import json
import pathlib
import os
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXPECTED = json.loads((ROOT / "test/reference/interoperability/ql-profile-host.json")
                      .read_text(encoding="utf-8"))


def manifest_payload(key):
    record = EXPECTED["profile"]["files"][key]
    data = record["pattern_utf8"].encode("utf-8") * record["repetitions"]
    if len(data) != record["bytes"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise AssertionError("QL independent profile manifest is inconsistent: " + key)
    return data


def check_manifest_file(path, key):
    record = EXPECTED["profile"]["files"][key]
    data = pathlib.Path(path).read_bytes()
    if len(data) != record["bytes"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise AssertionError("QL profile payload differs from independent manifest: " + key)


def run(binary, *args, cwd, env=None):
    command = [str(binary), *args]
    if pathlib.Path(binary).name == "bpmini":
        command.insert(0, "qemu-i386")
    return subprocess.run(command, cwd=cwd, capture_output=True, env=env)


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


def entry_times(path):
    archive = pathlib.Path(path).read_bytes()
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    count = struct.unpack_from("<I", archive, table_offset + 4)[0]
    record = table_offset + 16
    entries = {}
    for _ in range(count):
        name_length = struct.unpack_from("<H", archive, record)[0]
        name = archive[record + 2:record + 2 + name_length].decode("utf-8")
        fields = record + 2 + name_length
        entry_type = archive[fields + 16]
        mtime = struct.unpack_from("<Q", archive, fields + 18)[0]
        flags = struct.unpack_from("<H", archive, fields + 41)[0]
        if entry_type in (0, 3, 4):
            entries[name] = (mtime, flags)
        record = fields + 43
    return entries


def corrupt_first_payload(path):
    archive = bytearray(pathlib.Path(path).read_bytes())
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    record = table_offset + 16
    name_length = struct.unpack_from("<H", archive, record)[0]
    fields = record + 2 + name_length
    entry_offset = struct.unpack_from("<Q", archive, fields + 26)[0]
    inline_fields = entry_offset + 4 + name_length
    block_data = inline_fields + 38 + 12  # v3 block header precedes data
    archive[block_data] ^= 0x40
    pathlib.Path(path).write_bytes(archive)


def require_ok(result, label):
    assert result.returncode == 0, (
        f"{label} failed ({result.returncode}): "
        f"{result.stderr.decode(errors='replace')}"
    )


def check_dry_run(binary, archive, cwd):
    output_name = "dry-run-output"
    output = pathlib.Path(cwd) / output_name
    assert not output.exists()
    result = run(binary, "x", "-n", "-o", output_name, str(archive), cwd=cwd)
    require_ok(result, f"{pathlib.Path(binary).name} dry-run verification")
    assert not output.exists(), f"dry-run created {output}"


def main():
    ql, full, mini, collision_generator = (
        pathlib.Path(arg).resolve() for arg in sys.argv[1:5]
    )
    with tempfile.TemporaryDirectory(prefix="bp_ql_profile_") as temp:
        root = pathlib.Path(temp)
        (root / "first.bin").write_bytes(manifest_payload("first.bin"))
        (root / "second.bin").write_bytes(manifest_payload("second.bin"))

        result = run(ql, "c", "--interchange-profile-1", "profile.blt",
                     "first.bin", cwd=root)
        require_ok(result, "QL profile create")
        info = run(full, "i", "profile.blt", cwd=root)
        require_ok(info, "QL profile identity inspection")
        assert b"Creator OS:     Sinclair QL" in info.stdout
        assert b"Creator CPU:    Motorola 68k" in info.stdout
        profile = EXPECTED["profile"]
        assert entry_properties(root / "profile.blt")["first.bin"] == \
            (1, profile["block_bytes"])
        mtime, flags = entry_times(root / "profile.blt")["first.bin"]
        unknown = profile["unknown_mtime"]
        assert mtime == unknown["mtime"] and bool(flags & 0x4000) == unknown["valid"], \
            "QL file with unavailable mtime was marked as timestamped"
        check_dry_run(ql, "profile.blt", root)
        for reader in (ql, full, mini):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile create integrity with {reader.name}")

        result = run(ql, "a", "--interchange-profile-1", "profile.blt",
                     "second.bin", cwd=root)
        require_ok(result, "QL profile append")
        props = entry_properties(root / "profile.blt")
        assert props["first.bin"] == (1, profile["block_bytes"])
        assert props["second.bin"] == (1, profile["block_bytes"])
        for name, (mtime, flags) in entry_times(root / "profile.blt").items():
            if name.endswith(("first.bin", "second.bin")):
                assert mtime == unknown["mtime"] and \
                    bool(flags & 0x4000) == unknown["valid"], \
                    "QL append marked unavailable file time as valid"
        for reader_label, reader in (("ql", ql), ("linux", full), ("mini", mini)):
            require_ok(run(reader, "t", "profile.blt", cwd=root),
                       f"profile append integrity with {reader.name}")
            extracted = root / ("extract-" + reader_label)
            extracted.mkdir()
            if pathlib.Path(reader).name == "bpmini":
                extract_args = ("x", "profile.blt", extracted.name)
            else:
                extract_args = ("x", "-o", extracted.name, "profile.blt")
            require_ok(run(reader, *extract_args, cwd=root),
                       f"profile extraction with {pathlib.Path(reader).name}")
            check_manifest_file(extracted / "first.bin", "first.bin")
            check_manifest_file(extracted / "second.bin", "second.bin")

        result = run(ql, "c", "--interchange-profile-1", "-0",
                     "stored.blt", "first.bin", cwd=root)
        require_ok(result, "QL Stored profile create")
        assert entry_properties(root / "stored.blt")["first.bin"] == \
            (profile["stored_codec_id"], profile["block_bytes"])
        for reader in (ql, full, mini):
            require_ok(run(reader, "t", "stored.blt", cwd=root),
                       f"Stored profile integrity with {reader.name}")

        # QL extraction restores representable file dates.
        timed_tree = root / "TIMED" / "SUB"
        timed_tree.mkdir(parents=True)
        time_payload = EXPECTED["timestamps"]["timed_file_payload"]
        timed_data = time_payload["text"].encode("utf-8")
        if len(timed_data) != time_payload["bytes"] or \
                hashlib.sha256(timed_data).hexdigest() != time_payload["sha256"]:
            raise AssertionError("QL timestamp fixture manifest is inconsistent")
        (timed_tree / "LEAF").write_bytes(timed_data)
        timed_epoch = EXPECTED["timestamps"]["timed_file_epoch_seconds"]
        os.utime(timed_tree / "LEAF", (timed_epoch, timed_epoch))
        require_ok(run(full, "c", "-0", str(root / "timed-file.blt"),
                       "LEAF", cwd=timed_tree), "QL timestamp fixture create")
        timed_output = root / "ql-timed-output"
        timed_output.mkdir()
        result = run(ql, "x", "-o", timed_output.name, "timed-file.blt",
                     cwd=root)
        require_ok(result, "QL timestamp extraction")
        actual = int((timed_output / "LEAF").stat().st_mtime)
        assert actual == timed_epoch, \
            "QL extraction did not restore file mtime: %d" % actual
        future_epoch = EXPECTED["timestamps"]["unsupported_future_epoch_seconds"]
        os.utime(timed_tree / "LEAF", (future_epoch, future_epoch))
        require_ok(run(full, "c", "-0", str(root / "future-time.blt"),
                       "LEAF", cwd=timed_tree), "QL out-of-range time fixture")
        future_output = root / "ql-future-output"
        future_output.mkdir()
        result = run(ql, "x", "-o", future_output.name, "future-time.blt",
                     cwd=root)
        assert result.returncode != 0 and b"cannot restore QDOS file date" in result.stderr, \
            "QL did not report its unsupported timestamp range"
        assert (future_output / "LEAF").read_bytes() == timed_data, \
            "QL did not preserve verified bytes when the mtime was out of range"

        result = run(ql, "c", "--interchange-profile-1", "-F",
                     "rejected.blt", "first.bin", cwd=root)
        assert result.returncode != 0 and not (root / "rejected.blt").exists()

        # Extraction publishes only after checksum verification. A damaged
        # archive must leave an old forced target untouched and must not leave
        # a partially written file or staging sibling behind.
        require_ok(run(full, "c", "-0", "extract.blt", "first.bin", cwd=root),
                   "QL extraction fixture create")
        output_dir = root / "ql-extract"
        output_dir.mkdir()
        # The extractor must use exclusive stage creation and leave a
        # pre-existing sibling untouched when its first candidate collides.
        stage_collision = output_dir / "B0000000"
        stage_sentinel = b"pre-existing QL stage-name collision\n"
        stage_collision.write_bytes(stage_sentinel)
        require_ok(run(ql, "x", "-o", output_dir.name, "extract.blt", cwd=root),
                   "QL valid extraction")
        assert (output_dir / "first.bin").read_bytes() == (root / "first.bin").read_bytes()
        assert stage_collision.read_bytes() == stage_sentinel
        assert sorted(p.name for p in output_dir.iterdir()) == ["B0000000", "first.bin"]
        stage_collision.unlink()
        result = run(ql, "x", "-o", output_dir.name, "extract.blt", cwd=root)
        assert result.returncode != 0 and b"exists (use -f)" in result.stderr, \
            "QL no-force extraction did not refuse the existing file"
        assert (output_dir / "first.bin").read_bytes() == (root / "first.bin").read_bytes()

        corrupt = root / "corrupt-extract.blt"
        corrupt.write_bytes((root / "extract.blt").read_bytes())
        corrupt_first_payload(corrupt)
        sentinel = b"preserve existing QL target\n"
        (output_dir / "first.bin").write_bytes(sentinel)
        result = run(ql, "x", "-f", "-o", output_dir.name,
                     str(corrupt), cwd=root)
        assert result.returncode != 0 and b"checksum mismatch" in result.stderr, \
            "QL corrupt-payload case did not reach checksum verification: " + \
            result.stderr.decode(errors="replace")
        assert (output_dir / "first.bin").read_bytes() == sentinel
        assert sorted(p.name for p in output_dir.iterdir()) == ["first.bin"]

        # If publishing the verified stage fails after the existing target
        # was moved to its recovery name, the old bytes must be restored and
        # both temporary names removed.
        publish_failure_env = os.environ.copy()
        publish_failure_env["BP_QL_FAIL_PUBLISH"] = "1"
        (output_dir / "first.bin").write_bytes(sentinel)
        result = run(ql, "x", "-f", "-o", output_dir.name, "extract.blt",
                     cwd=root, env=publish_failure_env)
        assert result.returncode != 0 and b"cannot publish extracted file" in result.stderr, \
            "QL injected publication failure did not reach staged publish"
        assert (output_dir / "first.bin").read_bytes() == sentinel, \
            "QL failed publication did not restore the old target"
        assert sorted(p.name for p in output_dir.iterdir()) == ["first.bin"], \
            "QL failed publication left a stage or recovery backup"

        # If rollback also fails, keep both the old target's recovery copy and
        # the verified replacement stage visible so neither payload is lost.
        publish_failure_env["BP_QL_FAIL_ROLLBACK"] = "1"
        result = run(ql, "x", "-f", "-o", output_dir.name, "extract.blt",
                     cwd=root, env=publish_failure_env)
        assert result.returncode != 0 and b"recovery copy retained at:" in result.stderr, \
            "QL rollback failure did not report the retained recovery copy"
        assert not (output_dir / "first.bin").exists(), \
            "QL double publication failure unexpectedly left a target"
        retained = {p.name: p.read_bytes() for p in output_dir.iterdir()}
        backups = [data for name, data in retained.items() if name.startswith("K")]
        stages = [data for name, data in retained.items() if name.startswith("B")]
        assert backups == [sentinel], \
            "QL rollback failure did not retain the original target bytes"
        assert stages == [(root / "first.bin").read_bytes()], \
            "QL rollback failure did not retain the verified replacement stage"

        fresh_dir = root / "ql-fresh"
        fresh_dir.mkdir()
        result = run(ql, "x", "-o", fresh_dir.name, str(corrupt), cwd=root)
        assert result.returncode != 0 and b"checksum mismatch" in result.stderr, \
            "QL corrupt new-target case did not reach checksum verification"
        assert list(fresh_dir.iterdir()) == [], "failed QL extraction left visible output"

        # A user-selected output root plus a valid archive path can exceed the
        # QL frontend's fixed path buffer even though each part fits alone.
        # The frontend must refuse instead of extracting to a truncated name.
        (root / "nested").mkdir()
        (root / "nested" / "twenty-character-name.txt").write_bytes(b"path bound\n")
        require_ok(run(full, "c", "-0", "path-bound.blt",
                       "nested/twenty-character-name.txt", cwd=root),
                   "long-output-path fixture create")
        long_output = "o" * 250
        result = run(ql, "x", "-o", long_output, "path-bound.blt", cwd=root)
        assert result.returncode != 0 and b"output path is too long" in result.stderr
        assert not (root / long_output).is_dir()

        # QDOS filenames are generally case-insensitive. Reject aliases,
        # file/child conflicts, unmappable Latin-1 names and paths beyond the
        # native 36-byte bound before creating either the output root or the
        # QDOS directory marker used by the host build.
        expected_refusals = {
            key: value.encode("utf-8")
            for key, value in EXPECTED["expected_refusals"].items()
        }
        for scenario, diagnostic in expected_refusals.items():
            archive = root / ("ql-" + scenario + ".blt")
            require_ok(run(collision_generator, str(archive), scenario,
                           cwd=root), "QL " + scenario + " archive fixture")
            output = root / ("reject-" + scenario)
            result = run(ql, "x", "-o", str(output), str(archive), cwd=root)
            assert result.returncode != 0 and diagnostic in result.stderr, \
                "QL preflight failed to refuse %s: %s" % (
                    scenario, result.stderr.decode(errors="replace"))
            assert not output.exists(), "QL preflight created output root for " + scenario
            assert not pathlib.Path(str(output) + "_").exists(), \
                "QL preflight created directory marker for " + scenario

        exact36 = root / "ql-exact36.blt"
        require_ok(run(collision_generator, str(exact36), "exact36", cwd=root),
                   "QL exact 36-byte name fixture")
        require_ok(run(ql, "x", str(exact36), cwd=root),
                   "QL exact 36-byte native path extraction")
        assert (root / EXPECTED["names"]["exact_native_component"]).is_file(), \
            "QL rejected an exact 36-byte destination name"

    print("QL profile, staged extraction and destination preflight checks passed")


if __name__ == "__main__":
    main()
