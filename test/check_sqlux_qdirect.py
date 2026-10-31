#!/usr/bin/env python3
"""Probe sQLux hostFS qdirect records without assuming EOF behavior."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=120)
    if result.returncode != 0:
        raise RuntimeError("command failed: " + repr(command) + "\n" +
                           result.stdout + result.stderr)
    return result


def run_emulator(command, env, log_path, completion, timeout):
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True, env=env)
        deadline = time.monotonic() + timeout
        stable_since = None
        try:
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("sQLux exited before the expected output")
                if completion():
                    if stable_since is None:
                        stable_since = time.monotonic()
                    elif time.monotonic() - stable_since >= 0.4:
                        break
                else:
                    stable_since = None
                time.sleep(0.05)
            else:
                raise RuntimeError("timed out waiting for the QDOS command")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sqlux", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--boltpack", type=Path, required=True)
    parser.add_argument("--linux-boltpack", type=Path, required=True)
    parser.add_argument("--mini-boltpack", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    sqlux = args.sqlux.resolve(strict=True)
    rom = args.rom.resolve(strict=True)
    probe = args.probe.resolve(strict=True)
    boltpack = args.boltpack.resolve(strict=True)
    linux_boltpack = args.linux_boltpack.resolve(strict=True)
    mini_boltpack = args.mini_boltpack.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    shared = output / "shared"
    directory = shared / "QDTEST"
    (directory / "SUBDIR").mkdir(parents=True)
    (directory / "ONEFILE").write_bytes(b"one")
    (directory / "SUBDIR" / "TWOF").write_bytes(b"two")
    guest_probe = shared / "QPROBE"
    shutil.copyfile(probe, guest_probe)

    probe_command = [
        "xvfb-run", "-a", str(sqlux), "--romdir", str(rom.parent),
        "--sysrom", rom.name, "--ramtop", "1024", "--device",
        f"WIN1,{shared},qdos-like", "--boot_cmd", "EXEC_W win1_QPROBE",
    ]
    env = os.environ.copy()
    env["TZ"] = "UTC"
    result_file = shared / "QRES"
    run_emulator(probe_command, env, output / "probe-emulator.log",
                 lambda: result_file.is_file() and
                 "raw_read=" in result_file.read_text(encoding="ascii",
                                                      errors="replace"),
                 20)

    result = result_file.read_text(encoding="ascii", errors="replace")
    (output / "qdirect.txt").write_text(result, encoding="ascii")
    if "nested_file_read=3 first_byte=111" not in result:
        raise RuntimeError("hostFS direct nested-file access did not work")
    if "raw_read=64" not in result or "name=QDTEST_ONEFILE" not in result or \
            "name=QDTEST_SUBDIR" not in result or "type=255" not in result:
        raise RuntimeError("the Q_DIR channel did not return the independent file/directory records")

    guest_boltpack = shared / "BOLTPACK"
    shutil.copyfile(boltpack, guest_boltpack)
    source_archive = shared / "FROMDIR"
    create_command = [
        "xvfb-run", "-a", str(sqlux), "--romdir", str(rom.parent),
        "--sysrom", rom.name, "--ramtop", "1024", "--device",
        f"WIN1,{shared},qdos-like", "--boot_cmd",
        'EXEC_W win1_BOLTPACK;"c win1_FROMDIR win1_QDTEST_"',
    ]
    run_emulator(create_command, env, output / "create-emulator.log",
                 lambda: source_archive.is_file() and source_archive.stat().st_size > 0,
                 30)
    tested = subprocess.run([str(linux_boltpack), "t", str(source_archive)],
                            capture_output=True, text=True, timeout=60)
    mini_tested = subprocess.run([str(mini_boltpack), "t", str(source_archive)],
                                 capture_output=True, text=True, timeout=60)
    listed = subprocess.run([str(linux_boltpack), "l", str(source_archive)],
                            capture_output=True, text=True, timeout=60)
    (output / "archive-test.txt").write_text(
        tested.stdout + tested.stderr, encoding="utf-8")
    (output / "archive-list.txt").write_text(
        listed.stdout + listed.stderr, encoding="utf-8")
    (output / "archive-mini-test.txt").write_text(
        mini_tested.stdout + mini_tested.stderr, encoding="utf-8")
    if (tested.returncode != 0 or mini_tested.returncode != 0 or
            listed.returncode != 0 or "QDTEST/ONEFILE" not in listed.stdout or
            "QDTEST/SUBDIR/TWOF" not in listed.stdout or
            "2 files, 2 directories, 6 bytes" not in listed.stdout):
        raise RuntimeError("QDOS-created archive did not preserve the independent nested-tree manifest")

    linux_out = output / "linux-extracted"
    mini_out = output / "mini-extracted"
    linux_out.mkdir()
    mini_out.mkdir()
    linux_extract = run([str(linux_boltpack), "x", "-o", str(linux_out),
                         str(source_archive)], output)
    mini_extract = run([str(mini_boltpack), "x", str(source_archive),
                        str(mini_out)], output)
    (output / "linux-extract.txt").write_text(linux_extract.stdout, encoding="utf-8")
    (output / "mini-extract.txt").write_text(mini_extract.stdout, encoding="utf-8")
    expected = {"QDTEST/ONEFILE": b"one", "QDTEST/SUBDIR/TWOF": b"two"}
    for root in (linux_out, mini_out):
        actual = {path.relative_to(root).as_posix(): path.read_bytes()
                  for path in root.rglob("*") if path.is_file()}
        if actual != expected:
            raise RuntimeError("Linux/mini cross-extraction differs from independent nested-tree payloads")
        if not (root / "QDTEST/SUBDIR").is_dir():
            raise RuntimeError("Linux/mini cross-extraction omitted the nested directory")

    revision = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    status = subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"])
    diff = subprocess.check_output(["git", "-C", str(ROOT), "diff", "--binary"])
    (output / "git-status.txt").write_bytes(status)
    (output / "git-diff.patch").write_bytes(diff)

    manifest = {
        "result": "PASS",
        "claim": "QDOS Q_DIR/IO_FSTRG enumeration creates the independent nested tree; Linux full and mini readers validate and extract exact paths and bytes",
        "filesystem": "sQLux qdos-like hostFS",
        "source_revision": revision,
        "worktree_status_sha256": sha256_bytes(status),
        "tracked_diff_sha256": sha256_bytes(diff),
        "probe_command": probe_command,
        "create_command": create_command,
        "timezone": "UTC",
        "probe_sha256": sha256(probe),
        "ql_boltpack_sha256": sha256(boltpack),
        "linux_boltpack_sha256": sha256(linux_boltpack),
        "mini_boltpack_sha256": sha256(mini_boltpack),
        "sqlux_sha256": sha256(sqlux),
        "rom_sha256": sha256(rom),
        "host_file_sha256": sha256(directory / "ONEFILE"),
        "nested_file_sha256": sha256(directory / "SUBDIR" / "TWOF"),
        "qdirect_output_sha256": sha256(output / "qdirect.txt"),
        "archive_sha256": sha256(source_archive),
        "archive_list_sha256": sha256(output / "archive-list.txt"),
        "archive_mini_test_sha256": sha256(output / "archive-mini-test.txt"),
        "linux_extract_sha256": sha256(output / "linux-extract.txt"),
        "mini_extract_sha256": sha256(output / "mini-extract.txt"),
        "probe_emulator_log_sha256": sha256(output / "probe-emulator.log"),
        "create_emulator_log_sha256": sha256(output / "create-emulator.log"),
    }
    manifest["payloads"] = {
        "QDTEST/ONEFILE": {"size": len(expected["QDTEST/ONEFILE"]),
                           "sha256": sha256_bytes(expected["QDTEST/ONEFILE"])},
        "QDTEST/SUBDIR/TWOF": {"size": len(expected["QDTEST/SUBDIR/TWOF"]),
                               "sha256": sha256_bytes(expected["QDTEST/SUBDIR/TWOF"])},
    }
    manifest["artifacts"] = {
        str(path.relative_to(output)): {"size": path.stat().st_size, "sha256": sha256(path)}
        for path in sorted(output.rglob("*")) if path.is_file()
    }
    (output / "run.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                      encoding="utf-8")
    for name, item in manifest["artifacts"].items():
        path = output / name
        if path.stat().st_size != item["size"] or sha256(path) != item["sha256"]:
            raise RuntimeError("artifact hash mismatch: " + name)
    print("PASS: QDOS Q_DIR source enumeration and Linux/mini nested-tree transfer:", output)


if __name__ == "__main__":
    main()
