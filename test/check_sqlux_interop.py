#!/usr/bin/env python3
"""Exercise the QDOS frontend in sQLux and transfer archives with Linux."""
import argparse
import os
import pathlib
import shutil
import signal
import subprocess
import tempfile
import time


def run(command, *, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
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
    process = subprocess.Popen(
        emulator, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, start_new_session=True, env=env,
    )
    deadline = time.monotonic() + 30
    stable_since = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            output, _ = process.communicate()
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
                break
        else:
            stable_since = None
        time.sleep(0.1)
    else:
        os.killpg(process.pid, signal.SIGTERM)
        output, _ = process.communicate(timeout=5)
        raise RuntimeError(f"timed out waiting for QDOS command:\n{output}")
    if "using Minerva ROM" not in output:
        raise RuntimeError(f"sQLux did not report a successful ROM boot:\n{output}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sqlux", required=True, type=pathlib.Path)
    parser.add_argument("--rom", required=True, type=pathlib.Path)
    parser.add_argument("--ql-binary", required=True, type=pathlib.Path)
    parser.add_argument("--linux-binary", required=True, type=pathlib.Path)
    parser.add_argument("--mini-binary", required=True, type=pathlib.Path)
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

    payload = bytes(range(256)) * 3 + b"QDOS interoperability\n"
    with tempfile.TemporaryDirectory(prefix="boltpack-sqlux-") as temp:
        root = pathlib.Path(temp)
        shared = root / "shared"
        source = root / "source"
        linux_out = root / "linux-out"
        shared.mkdir()
        source.mkdir()
        linux_out.mkdir()
        shutil.copyfile(ql_binary, shared / "BOLTPACK")

        (shared / "INPUT").write_bytes(payload)
        ql_archive = shared / "from_ql"
        run_ql(sqlux, rom, shared, "c -0 win1_from_ql win1_INPUT",
               lambda: ql_archive.is_file() and ql_archive.stat().st_size > 0)
        run([str(linux_binary), "t", str(shared / "from_ql")])
        run(["qemu-i386", str(mini_binary), "t", str(shared / "from_ql")])
        run([str(linux_binary), "x", "-o", str(linux_out),
             str(shared / "from_ql")])
        if (linux_out / "INPUT").read_bytes() != payload:
            raise AssertionError("Linux did not recover the QDOS-created payload")

        (source / "RESULT").write_bytes(payload)
        run([str(linux_binary), "c", "-0", str(shared / "to_ql"), "RESULT"],
            cwd=source)
        (shared / "B0000000").write_bytes(b"stage collision sentinel\n")
        result = shared / "RESULT"
        run_ql(sqlux, rom, shared, "x win1_to_ql",
               lambda: result.is_file() and result.read_bytes() == payload)
        if (shared / "RESULT").read_bytes() != payload:
            raise AssertionError("QDOS extraction payload differs")
        if (shared / "B0000000").read_bytes() != b"stage collision sentinel\n":
            raise AssertionError("QDOS extraction overwrote the occupied stage name")
        if (shared / "B0000001").exists():
            raise AssertionError("QDOS extraction left a stage file behind")

        result.write_bytes(b"existing QDOS destination\n")
        run_ql(sqlux, rom, shared, "x -f win1_to_ql",
               lambda: result.is_file() and result.read_bytes() == payload)
        if result.read_bytes() != payload:
            raise AssertionError("forced QDOS replacement payload differs")
        if (shared / "K0000001").exists():
            raise AssertionError("forced QDOS replacement left a backup")

        (source / "OUTDATA").write_bytes(payload)
        run([str(linux_binary), "c", "-0", str(shared / "to_ql_out"), "OUTDATA"],
            cwd=source)
        output_root_file = shared / "OUTDIR_OUTDATA"
        run_ql(sqlux, rom, shared, "x -o win1_OUTDIR win1_to_ql_out",
               lambda: output_root_file.is_file() and
               output_root_file.read_bytes() == payload)
        if output_root_file.read_bytes() != payload:
            raise AssertionError("QDOS -o extraction payload differs")
        if not (shared / "OUTDIR_").is_file():
            raise AssertionError("QDOS -o extraction did not create its root")

        tree = source / "TREE" / "SUB"
        tree.mkdir(parents=True)
        (tree / "LEAF").write_bytes(payload)
        os.utime(tree / "LEAF", (1600000003, 1600000003))
        os.utime(tree, (1600000002, 1600000002))
        os.utime(source / "TREE", (1600000001, 1600000001))
        run([str(linux_binary), "c", "-0", str(shared / "nested"), "TREE"],
            cwd=source)
        nested_leaf = shared / "TREE_SUB_LEAF"
        run_ql(sqlux, rom, shared, "x win1_nested",
               lambda: nested_leaf.is_file() and nested_leaf.read_bytes() == payload)
        if nested_leaf.read_bytes() != payload:
            raise AssertionError("nested QDOS extraction payload differs")
        if not (shared / "TREE_").is_file() or not (shared / "TREE_SUB_").is_file():
            raise AssertionError("nested QDOS extraction did not create directories")
        expected_times = {
            "TREE_": 1600000001,
            "TREE_SUB_": 1600000002,
            "TREE_SUB_LEAF": 1600000003,
        }
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
        os.utime(offset_source, (1600000000, 1600000000))
        run([str(linux_binary), "c", "-0", str(shared / "offset_archive"), "OFFSET"],
            cwd=source)
        offset_output = shared / "OFFSET"
        run_ql(sqlux, rom, shared,
               "x --tz-offset 60 win1_offset_archive",
               lambda: offset_output.is_file() and
               offset_output.read_bytes() == payload)
        actual = int(offset_output.stat().st_mtime)
        if actual != 1600003600:
            raise AssertionError(
                f"QDOS +01:00 timestamp is {actual}, expected 1600003600"
            )

    print("sQLux QDOS create/extract, UTC and +01:00 timestamps, forced replacement, -o paths, stage collision, nested paths, and Linux transfer passed")


if __name__ == "__main__":
    main()
