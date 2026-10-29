#!/usr/bin/env python3
"""Inject Linux CLI extraction output failures and check staged preservation."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True, type=Path)
    args = parser.parse_args()
    boltpack = args.boltpack.resolve()
    interposer_source = Path(__file__).with_name(
        "linux_stage_failure_interposer.c")

    with tempfile.TemporaryDirectory(prefix="boltpack-linux-extract-failures-") as temp:
        root = Path(temp)
        source = root / "source"
        source.mkdir()
        payload = source / "payload.bin"
        payload.write_bytes(bytes(range(256)) * 4096)
        os.utime(payload, (1_700_000_000, 1_700_000_000))
        archive = root / "fixture.bolt"
        created = subprocess.run(
            [str(boltpack), "c", "-0", str(archive), payload.name],
            cwd=source, capture_output=True)
        if created.returncode != 0:
            raise SystemExit("could not create extraction failure fixture: " +
                             created.stderr.decode(errors="replace"))

        interposer = root / "stage-failures.so"
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC",
                        str(interposer_source), "-ldl", "-o", str(interposer)],
                       check=True)

        failures = {
            "FWRITE": "write",
            "FFLUSH": "flush",
            "FCLOSE": "close",
            "FUTIMENS": "timestamp restoration",
        }
        for variable, label in failures.items():
            output = root / f"output-{variable.lower()}"
            output.mkdir()
            old = output / payload.name
            old_bytes = f"keep original data after {label} failure\n".encode()
            old.write_bytes(old_bytes)
            env = os.environ.copy()
            env["LD_PRELOAD"] = str(interposer)
            env[f"BOLTPACK_TEST_FAIL_{variable}"] = "1"
            result = subprocess.run(
                [str(boltpack), "x", "-f", "-o", str(output), str(archive)],
                cwd=root, env=env, capture_output=True)
            if result.returncode == 0:
                raise SystemExit(f"injected {label} failure was accepted")
            if old.read_bytes() != old_bytes:
                raise SystemExit(f"{label} failure replaced the existing file")
            leftovers = [entry.name for entry in output.iterdir()
                         if entry.name.startswith(".boltpack-")]
            if leftovers:
                raise SystemExit(f"{label} failure left stages: {leftovers!r}")

        tree = root / "tree"
        directory = tree / "folder"
        directory.mkdir(parents=True)
        directory_payload = directory / "child.bin"
        directory_bytes = b"child data survives deferred directory metadata failure\n"
        directory_payload.write_bytes(directory_bytes)
        os.utime(directory_payload, (1_650_000_000, 1_650_000_000))
        os.utime(directory, (1_600_000_000, 1_600_000_000))
        directory_archive = root / "directory-metadata.bolt"
        created = subprocess.run(
            [str(boltpack), "c", "-0", str(directory_archive), "folder"],
            cwd=tree, capture_output=True)
        if created.returncode != 0:
            raise SystemExit("could not create directory metadata fixture: " +
                             created.stderr.decode(errors="replace"))
        directory_output = root / "directory-output"
        env = os.environ.copy()
        env["LD_PRELOAD"] = str(interposer)
        env["BOLTPACK_TEST_FAIL_DIR_FUTIMENS"] = "1"
        result = subprocess.run(
            [str(boltpack), "x", "-o", str(directory_output),
             str(directory_archive)], cwd=root, env=env, capture_output=True)
        if result.returncode == 0:
            raise SystemExit("deferred directory timestamp failure was accepted")
        extracted_child = directory_output / "folder" / "child.bin"
        if not extracted_child.is_file() or extracted_child.read_bytes() != directory_bytes:
            raise SystemExit("directory metadata failure lost verified child data")
        leftovers = [path.name for path in (directory_output / "folder").iterdir()
                     if path.name.startswith(".boltpack-")]
        if leftovers:
            raise SystemExit("directory metadata failure left stages: " +
                             repr(leftovers))

    print("Linux CLI file write/flush/close/timestamp and deferred-directory timestamp failures have explicit outcomes and clean stages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
