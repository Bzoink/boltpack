#!/usr/bin/env python3
"""Exercise the Win64 WCX Unicode exports through Wine's DLL loader."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command, *, env=None, cwd=None, expected=(0,)):
    result = subprocess.run(command, capture_output=True, text=True, env=env,
                            cwd=cwd, timeout=90)
    if result.returncode not in expected:
        raise RuntimeError("command failed (%d): %r\n%s%s" %
                           (result.returncode, command, result.stdout,
                            result.stderr))
    return result.stdout.strip()


def wine_path(path, env):
    return run(["winepath", "-w", str(path)], env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", default="wine64")
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--wcx", type=Path, required=True)
    parser.add_argument("--linux-boltpack", type=Path, required=True)
    parser.add_argument("--keep", action="store_true",
                        help="retain the temporary fixture even if a check fails")
    args = parser.parse_args()
    runner = args.runner.resolve(strict=True)
    plugin = args.wcx.resolve(strict=True)
    linux = args.linux_boltpack.resolve(strict=True)
    env = os.environ.copy()
    env["WINEDEBUG"] = "-all"
    drive_c = Path(run(["winepath", "-u", "C:\\"], env=env))
    drive_c.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="boltpack-wcx-wine-", dir=drive_c))
    try:
        source = root / "source"
        (source / "nested").mkdir(parents=True)
        payload = ("WCX Unicode transfer: 東京 🙂\n" * 19).encode("utf-8")
        (source / "nested" / "東京-🙂.txt").write_bytes(payload)
        pack_source = root / "pack-source" / "nested"
        pack_source.mkdir(parents=True)
        (pack_source / "東京-🙂.txt").write_bytes(payload)
        append_payload = b"WCX Unicode append transfer\n" * 7
        (root / "pack-source" / "extra-東京.txt").write_bytes(append_payload)
        archive = root / "archive-💾.blt"
        run([str(linux), "c", "-0", str(archive),
             "nested/東京-🙂.txt"], env=env, cwd=source)
        shutil.copyfile(archive, root / "archive-ascii.blt")

        directory_source = root / "directory-source"
        nested_source = directory_source / "nested"
        nested_source.mkdir(parents=True)
        (nested_source / "child.txt").write_bytes(b"directory finalization\n")
        directory_mtime = 946684800
        os.utime(nested_source, (directory_mtime, directory_mtime))
        directory_archive = root / "directory-archive.blt"
        run([str(linux), "c", "-0", str(directory_archive), "nested"],
            env=env, cwd=directory_source)

        win_runner = wine_path(runner, env)
        win_plugin = wine_path(plugin, env)
        win_root = wine_path(root, env)
        runner_output = run([args.wine, win_runner, win_plugin, win_root], env=env)

        run([str(linux), "t", str(archive)], env=env)
        deleted_listing = run([str(linux), "l", str(archive)], env=env)
        if "0 files" not in deleted_listing:
            raise RuntimeError("DeleteFilesW left entries in the Linux-created archive")
        extracted = root / "extract" / "nested" / "東京-🙂.txt"
        if not extracted.is_file() or extracted.read_bytes() != payload:
            raise RuntimeError("WCX Unicode extraction bytes differ from source")
        packed_archive = root / "packed-💾.blt"
        run([str(linux), "t", str(packed_archive)], env=env)
        packed_output = root / "packed-linux"
        packed_output.mkdir()
        run([str(linux), "x", "-o", str(packed_output), str(packed_archive)],
            env=env, expected=(0, 1))
        packed_file = packed_output / "extra-東京.txt"
        if not packed_file.is_file() or packed_file.read_bytes() != append_payload:
            raise RuntimeError("Linux could not recover exact bytes after WCX append")
        if (packed_output / "nested" / "東京-🙂.txt").exists():
            raise RuntimeError("DeleteFilesW did not remove the selected Unicode entry")
        extracted_directory = root / "directory-extract" / "nested"
        if not extracted_directory.is_dir():
            raise RuntimeError("WCX did not extract the nested directory")
        if int(extracted_directory.stat().st_mtime) != directory_mtime:
            raise RuntimeError("WCX did not restore the directory timestamp at close")
        print(runner_output)
        print("PASS: Linux verified WCX create/append/delete, directory timestamp finalization, bytes, and final entries")
    finally:
        if args.keep:
            print("fixture retained:", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
