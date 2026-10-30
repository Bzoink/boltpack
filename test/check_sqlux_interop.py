#!/usr/bin/env python3
"""Exercise the QDOS frontend in sQLux and transfer archives with Linux."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXPECTED = json.loads((ROOT / "test/reference/interoperability/ql-sqlux.json")
                      .read_text(encoding="utf-8"))
PAYLOAD_CASE = EXPECTED["payload"]
PAYLOAD = bytes(range(256)) * 3 + PAYLOAD_CASE["suffix_utf8"].encode("utf-8")
EVIDENCE_LOG = None


def record(text):
    if EVIDENCE_LOG is not None:
        with EVIDENCE_LOG.open("a", encoding="utf-8") as stream:
            stream.write(text + "\n")


def check_payload(data, label):
    if len(data) != PAYLOAD_CASE["bytes"] or \
            hashlib.sha256(data).hexdigest() != PAYLOAD_CASE["sha256"]:
        raise AssertionError("independent QL manifest mismatch: " + label)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_version(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=10)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable: " + str(exc)
    output = (result.stdout + result.stderr).strip().splitlines()
    return output[0] if output else "unknown"


def run(command, *, cwd=None):
    record("command=" + repr(command) + " cwd=" + repr(str(cwd) if cwd else None))
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    record("exit=" + str(result.returncode) + "\nstdout:\n" + result.stdout +
           "\nstderr:\n" + result.stderr)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def run_ql(sqlux, rom, shared, command, completion):
    emulator = [
        "xvfb-run", "-a", str(sqlux),
        "--romdir", str(rom.parent), "--sysrom", rom.name,
        "--ramtop", "1024", "--device", f"WIN1,{shared},qdos-like",
        "--boot_cmd", f'EXEC_W win1_BOLTPACK;"{command}"',
    ]
    env = os.environ.copy()
    # QDOS stores wall-clock seconds without a timezone. Use UTC so hostFS
    # does not apply the host's local offset to the archive's UTC timestamps.
    env["TZ"] = "UTC"
    env.pop("BP_TZ_OFFSET", None)
    record("emulator=" + repr(emulator) + " TZ=UTC")
    process = subprocess.Popen(
        emulator, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, start_new_session=True, env=env,
    )
    deadline = time.monotonic() + 30
    stable_since = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            output, _ = process.communicate()
            record("emulator exit=" + str(process.returncode) + "\n" + output)
            if process.returncode != 0:
                raise RuntimeError(
                    f"sQLux exited unexpectedly ({process.returncode}):\n{output}"
                )
            break
        if completion():
            if stable_since is None:
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= 0.4:
                os.killpg(process.pid, signal.SIGTERM)
                output, _ = process.communicate(timeout=5)
                record("emulator stopped after output stabilized\n" + output)
                break
        else:
            stable_since = None
        time.sleep(0.1)
    else:
        os.killpg(process.pid, signal.SIGTERM)
        output, _ = process.communicate(timeout=5)
        record("emulator timed out\n" + output)
        raise RuntimeError(f"timed out waiting for QDOS command:\n{output}")
    if "using Minerva ROM" not in output:
        raise RuntimeError(f"sQLux did not report a successful ROM boot:\n{output}")


def main():
    global EVIDENCE_LOG
    parser = argparse.ArgumentParser()
    parser.add_argument("--sqlux", required=True, type=pathlib.Path)
    parser.add_argument("--rom", required=True, type=pathlib.Path)
    parser.add_argument("--ql-binary", required=True, type=pathlib.Path)
    parser.add_argument("--linux-binary", required=True, type=pathlib.Path)
    parser.add_argument("--mini-binary", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path,
                        help="retain run artifacts and machine-readable evidence here")
    args = parser.parse_args()
    sqlux = args.sqlux.resolve()
    rom = args.rom.resolve()
    ql_binary = args.ql_binary.resolve()
    linux_binary = args.linux_binary.resolve()
    mini_binary = args.mini_binary.resolve()
    for path in (sqlux, rom, ql_binary, linux_binary, mini_binary):
        if not path.is_file():
            raise SystemExit(f"required file not found: {path}")
    if shutil.which("xvfb-run") is None:
        raise SystemExit("xvfb-run is required")

    payload = PAYLOAD
    check_payload(payload, "generated source")
    names = EXPECTED["names"]
    if args.output:
        root = args.output.resolve()
        root.mkdir(parents=True, exist_ok=False)
        temp_context = None
    else:
        temp_context = tempfile.TemporaryDirectory(prefix="boltpack-sqlux-")
        root = pathlib.Path(temp_context.name)
    EVIDENCE_LOG = root / "commands.log"
    record("invocation=" + repr(sys.argv))
    (root / "git-status.txt").write_bytes(subprocess.check_output(
        ["git", "-C", str(ROOT), "status", "--porcelain"]))
    (root / "git-diff.patch").write_bytes(subprocess.check_output(
        ["git", "-C", str(ROOT), "diff", "--binary", "HEAD"]))
    shutil.copyfile(ROOT / "test/reference/interoperability/ql-sqlux.json",
                    root / "expected.json")
    shutil.copyfile(pathlib.Path(__file__), root / "runner.py")
    try:
        shared = root / "shared"
        source = root / "source"
        linux_out = root / "linux-out"
        shared.mkdir()
        source.mkdir()
        linux_out.mkdir()
        shutil.copyfile(ql_binary, shared / "BOLTPACK")

        (shared / names["ql_input"]).write_bytes(payload)
        ql_archive = shared / names["ql_created_archive"]
        run_ql(sqlux, rom, shared,
               "c -0 win1_%s win1_%s" %
               (names["ql_created_archive"], names["ql_input"]),
               lambda: ql_archive.is_file() and ql_archive.stat().st_size > 0)
        run([str(linux_binary), "t", str(ql_archive)])
        run(["qemu-i386", str(mini_binary), "t", str(ql_archive)])
        run([str(linux_binary), "x", "-o", str(linux_out),
             str(ql_archive)])
        if (linux_out / names["ql_input"]).read_bytes() != payload:
            raise AssertionError("Linux did not recover the QDOS-created payload")
        check_payload((linux_out / names["ql_input"]).read_bytes(), "Linux restore")

        (source / "RESULT").write_bytes(payload)
        ql_target_archive = shared / names["linux_created_archive"]
        run([str(linux_binary), "c", "-0", str(ql_target_archive), "RESULT"],
            cwd=source)
        (shared / "B0000000").write_bytes(b"stage collision sentinel\n")
        result = shared / "RESULT"
        run_ql(sqlux, rom, shared,
               "x win1_" + names["linux_created_archive"],
               lambda: result.is_file() and result.read_bytes() == payload)
        if (shared / "RESULT").read_bytes() != payload:
            raise AssertionError("QDOS extraction payload differs")
        check_payload((shared / "RESULT").read_bytes(), "QDOS restore")
        if (shared / "B0000000").read_bytes() != b"stage collision sentinel\n":
            raise AssertionError("QDOS extraction overwrote the occupied stage name")
        if (shared / "B0000001").exists():
            raise AssertionError("QDOS extraction left a stage file behind")

        result.write_bytes(b"existing QDOS destination\n")
        run_ql(sqlux, rom, shared,
               "x -f win1_" + names["linux_created_archive"],
               lambda: result.is_file() and result.read_bytes() == payload)
        if result.read_bytes() != payload:
            raise AssertionError("forced QDOS replacement payload differs")
        if (shared / "K0000001").exists():
            raise AssertionError("forced QDOS replacement left a backup")

        (source / names["flat_output"]).write_bytes(payload)
        run([str(linux_binary), "c", "-0",
             str(shared / names["linux_flat_archive"]), names["flat_output"]],
            cwd=source)
        output_root_file = shared / names["flat_output_file"]
        run_ql(sqlux, rom, shared,
               "x -o win1_%s win1_%s" %
               (names["flat_output_directory"], names["linux_flat_archive"]),
               lambda: output_root_file.is_file() and
               output_root_file.read_bytes() == payload)
        if output_root_file.read_bytes() != payload:
            raise AssertionError("QDOS -o extraction payload differs")
        check_payload(output_root_file.read_bytes(), "QDOS output-root restore")
        if not (shared / names["flat_output_directory"]).is_file():
            raise AssertionError("QDOS -o extraction did not create its root")

        tree = source / "TREE" / "SUB"
        tree.mkdir(parents=True)
        (tree / "LEAF").write_bytes(payload)
        times = EXPECTED["timestamps"]
        root_time, sub_time = times["nested_directories_epoch_seconds"]
        file_time = times["nested_file_epoch_seconds"]
        os.utime(tree / "LEAF", (file_time, file_time))
        os.utime(tree, (sub_time, sub_time))
        os.utime(source / "TREE", (root_time, root_time))
        run([str(linux_binary), "c", "-0", str(shared / names["nested_archive"]), "TREE"],
            cwd=source)
        nested_leaf = shared / names["nested_qdos_file"]
        run_ql(sqlux, rom, shared, "x win1_" + names["nested_archive"],
               lambda: nested_leaf.is_file() and nested_leaf.read_bytes() == payload)
        if nested_leaf.read_bytes() != payload:
            raise AssertionError("nested QDOS extraction payload differs")
        check_payload(nested_leaf.read_bytes(), "QDOS nested restore")
        if not (shared / names["nested_qdos_root"]).is_file() or \
                not (shared / names["nested_qdos_subdirectory"]).is_file():
            raise AssertionError("nested QDOS extraction did not create directories")
        expected_times = dict(zip(
            (names["nested_qdos_root"], names["nested_qdos_subdirectory"],
             names["nested_qdos_file"]),
            [root_time, sub_time, file_time]))
        for name, expected in expected_times.items():
            actual = int((shared / name).stat().st_mtime)
            if actual != expected:
                raise AssertionError(
                    f"QDOS extraction mtime for {name} is {actual}, expected {expected}"
                )

        # QDOS stores local wall time. Verify an explicit +01:00 offset is
        # applied when restoring an archive UTC time.
        offset_source = source / "OFFSET"
        offset_source.write_bytes(payload)
        source_epoch = times["source_epoch_seconds"]
        os.utime(offset_source, (source_epoch, source_epoch))
        offset_archive = shared / names["offset_archive"]
        run([str(linux_binary), "c", "-0", str(offset_archive), "OFFSET"],
            cwd=source)
        offset_output = shared / "OFFSET"
        run_ql(sqlux, rom, shared,
               "x --tz-offset %d win1_%s" %
               (times["explicit_offset_minutes"], names["offset_archive"]),
               lambda: offset_output.is_file() and
               offset_output.read_bytes() == payload)
        actual = int(offset_output.stat().st_mtime)
        if actual != times["expected_offset_file_epoch_seconds"]:
            raise AssertionError(
                f"QDOS +01:00 timestamp is {actual}, expected "
                f"{times['expected_offset_file_epoch_seconds']}"
            )

        for offset_case in times["additional_offset_cases"]:
            archive_name = names[offset_case["archive"] + "_archive"]
            offset_archive = shared / archive_name
            run([str(linux_binary), "c", "-0", str(offset_archive), "OFFSET"],
                cwd=source)
            offset_output.unlink()
            run_ql(sqlux, rom, shared,
                   "x --tz-offset %d win1_%s" %
                   (offset_case["minutes"], archive_name),
                   lambda: offset_output.is_file() and
                   offset_output.read_bytes() == payload)
            actual = int(offset_output.stat().st_mtime)
            if actual != offset_case["expected_epoch_seconds"]:
                raise AssertionError(
                    "QDOS timezone offset %d minutes restored %d, expected %d" %
                    (offset_case["minutes"], actual,
                     offset_case["expected_epoch_seconds"])
                )
        git_revision = command_version(["git", "-C", str(ROOT), "rev-parse", "HEAD"])
        git_status = subprocess.run(
            ["git", "-C", str(ROOT), "status", "--porcelain"],
            capture_output=True, text=True, check=True)
        archive_names = {value for key, value in names.items()
                         if key.endswith("_archive")}
        artifacts = {}
        for archive_name in sorted(archive_names):
            archive = shared / archive_name
            artifacts[archive.name] = {
                "bytes": archive.stat().st_size,
                "sha256": sha256_file(archive),
            }
        evidence = {
            "status": "PASS",
            "reference_manifest": "test/reference/interoperability/ql-sqlux.json",
            "reference_manifest_sha256": sha256_file(
                ROOT / "test/reference/interoperability/ql-sqlux.json"),
            "revision": git_revision,
            "worktree_dirty": bool(git_status.stdout.strip()),
            "invocation": sys.argv,
            "runner_sha256": sha256_file(root / "runner.py"),
            "command_log_sha256": sha256_file(root / "commands.log"),
            "host": platform.platform(),
            "locale": os.environ.get("LC_ALL") or os.environ.get("LC_CTYPE") or "",
            "timezone": "UTC",
            "emulator": command_version([str(sqlux), "--version"]),
            "rom": {"path": str(rom), "sha256": sha256_file(rom)},
            "binaries": {
                "ql": {"path": str(ql_binary), "sha256": sha256_file(ql_binary)},
                "linux": {"path": str(linux_binary), "sha256": sha256_file(linux_binary)},
                "mini": {"path": str(mini_binary), "sha256": sha256_file(mini_binary)},
            },
            "artifacts": artifacts,
            "checks": ["QDOS-created archive read by Linux full and mini",
                       "Linux-created archive extracted by QDOS",
                       "forced replacement and stage collision",
                       "output-root and nested paths",
                       "UTC timestamps and -300, +60, +330 minute offsets"],
        }
        (root / "manifest.json").write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    finally:
        if temp_context is not None:
            temp_context.cleanup()

    if args.output:
        print("sQLux QDOS create/extract, UTC and timezone-offset timestamps, forced replacement, -o paths, stage collision, nested paths, and Linux transfer passed; evidence: %s" % root)
    else:
        print("sQLux QDOS create/extract, UTC and timezone-offset timestamps, forced replacement, -o paths, stage collision, nested paths, and Linux transfer passed")


if __name__ == "__main__":
    main()
