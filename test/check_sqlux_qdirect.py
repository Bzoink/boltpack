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
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    sqlux = args.sqlux.resolve(strict=True)
    rom = args.rom.resolve(strict=True)
    probe = args.probe.resolve(strict=True)
    boltpack = args.boltpack.resolve(strict=True)
    linux_boltpack = args.linux_boltpack.resolve(strict=True)
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
    if "stdio_read=64" not in result or "name_length=0 name=" not in result:
        raise RuntimeError("the stdio qdirect empty-name record was not observed")
    if "raw_read=65535" not in result:
        raise RuntimeError("the raw QDOS qdirect EOF result changed; inspect qdirect.txt")

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
    listed = subprocess.run([str(linux_boltpack), "l", str(source_archive)],
                            capture_output=True, text=True, timeout=60)
    (output / "archive-test.txt").write_text(
        tested.stdout + tested.stderr, encoding="utf-8")
    (output / "archive-list.txt").write_text(
        listed.stdout + listed.stderr, encoding="utf-8")
    if tested.returncode != 0 or listed.returncode != 0 or \
            "0 files, 1 directory" not in listed.stdout:
        raise RuntimeError("QDOS create did not terminate with the observed incomplete directory-only archive")

    revision = subprocess.check_output(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    status = subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"])
    diff = subprocess.check_output(["git", "-C", str(ROOT), "diff", "--binary"])
    (output / "git-status.txt").write_bytes(status)
    (output / "git-diff.patch").write_bytes(diff)

    manifest = {
        "result": "LIMITATION_REPRODUCED_FRONTEND_TERMINATES",
        "claim": "sQLux hostFS opens nested files but returns an empty qdirect record; QDOS create therefore cannot enumerate this populated directory",
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
        "sqlux_sha256": sha256(sqlux),
        "rom_sha256": sha256(rom),
        "host_file_sha256": sha256(directory / "ONEFILE"),
        "nested_file_sha256": sha256(directory / "SUBDIR" / "TWOF"),
        "qdirect_output_sha256": sha256(output / "qdirect.txt"),
        "archive_sha256": sha256(source_archive),
        "archive_list_sha256": sha256(output / "archive-list.txt"),
        "probe_emulator_log_sha256": sha256(output / "probe-emulator.log"),
        "create_emulator_log_sha256": sha256(output / "create-emulator.log"),
    }
    (output / "run.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                      encoding="utf-8")
    print("PASS: qdirect terminator no longer loops; reproduced sQLux hostFS directory omission, so recursive source qualification remains open:", output)


if __name__ == "__main__":
    main()
