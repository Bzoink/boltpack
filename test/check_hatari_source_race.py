#!/usr/bin/env python3
"""Verify Atari CLI refuses a source path replaced between stat and open."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
ORIGINAL = b"original source captured before the path swap\r\n"
REPLACEMENT = b"replacement has a different length and must not be archived\r\n"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact_index(root):
    return {
        path.relative_to(root).as_posix(): {
            "size": path.stat().st_size,
            "sha256": sha256(path),
        }
        for path in sorted(root.rglob("*"))
        if path.is_file() and path != root / "run.json"
    }


def command_version(command):
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10,
                                check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable: " + str(exc)
    return result.stdout.decode("utf-8", "replace").strip().splitlines()[:3]


def wait_for(path, process, timeout, description, nonempty=False):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and (not nonempty or path.stat().st_size > 0):
            return
        if process.poll() is not None:
            raise RuntimeError("Hatari exited before " + description)
        time.sleep(0.05)
    raise RuntimeError("timed out waiting for " + description)


def wait_for_lines(path, process, timeout, count):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and len(path.read_text(encoding="ascii").splitlines()) >= count:
            return
        if process.poll() is not None:
            raise RuntimeError("Hatari exited before guest command results were written")
        time.sleep(0.05)
    raise RuntimeError("timed out waiting for guest command results")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", required=True, type=Path)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--cc", default="m68k-atari-mint-gcc")
    parser.add_argument("--atari-cli", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(
        tempfile.mkdtemp(prefix="atari-source-race-", dir=ROOT / "build"))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    drive = output / "drive"
    drive.mkdir()
    tos = args.tos.expanduser().resolve(strict=True)
    atari_cli = args.atari_cli.resolve(strict=True)
    shutil.copyfile(atari_cli, drive / "BOLTPACK.TOS")
    guest = ROOT / "test/atari/guest_runner.c"
    runner = drive / "RUN.TOS"
    compile_result = subprocess.run(
        [args.cc, "-O2", str(guest), "-o", str(runner)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    (output / "compile.log").write_bytes(compile_result.stdout)
    if compile_result.returncode != 0:
        raise RuntimeError("could not compile Hatari guest runner; see " +
                           str(output / "compile.log"))

    source = drive / "SOURCE.TXT"
    replacement = drive / "REPLACE.TXT"
    source.write_bytes(ORIGINAL)
    replacement.write_bytes(REPLACEMENT)
    replacement_fixture = output / "replacement-input.bin"
    replacement_fixture.write_bytes(REPLACEMENT)
    (drive / "COMMANDS.TXT").write_text(
        "c RACE1.BLT SOURCE.TXT\nc RACE2.BLT SOURCE.TXT\n",
                                       encoding="ascii")
    log_path = output / "hatari.log"
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    command = [args.hatari, "--machine", "ste", "--tos", str(tos),
               "--sound", "off", "--confirm-quit", "off",
               "--fast-forward", "on", "--fast-boot", "on", "--memsize", "4",
               "--harddrive", str(drive), "--auto", r"C:\RUN.TOS",
               "--run-vbls", "16000"]
    with log_path.open("wb") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(drive / "STATDONE.TXT", process, 90,
                     "the source-stat pause marker")
            before_stat_swap = source.stat()
            parked = drive / "ORIGINAL.TXT"
            os.replace(source, parked)
            os.replace(replacement, source)
            after_stat_swap = source.stat()
            (drive / "CONTINUE.TXT").write_text("continue\n", encoding="ascii")
            wait_for_lines(drive / "RESULT.TXT", process, 90, 1)
            if "source changed while opening" not in \
                    (drive / "LOG00.TXT").read_text(errors="replace"):
                raise RuntimeError("stat/open substitution was not rejected; see LOG00.TXT")

            os.replace(source, replacement)
            os.replace(parked, source)
            wait_for(drive / "OPENED.TXT", process, 90,
                     "the source-open pause marker")
            before_open_swap = source.stat()
            parked_after_open = drive / "ORIG2.TXT"
            os.replace(source, parked_after_open)
            shutil.copyfile(replacement_fixture, source)
            after_open_swap = source.stat()
            (drive / "CONT2.TXT").write_text("continue\n", encoding="ascii")
            wait_for_lines(drive / "RESULT.TXT", process, 90, 2)
            result = (drive / "RESULT.TXT").read_text(encoding="ascii").splitlines()
            if result != ["1", "1"]:
                raise RuntimeError("Atari create did not reject both source substitutions: " +
                                   repr(result))
            if "source changed while reading" not in \
                    (drive / "LOG01.TXT").read_text(errors="replace"):
                raise RuntimeError("open/path substitution was not rejected; see LOG01.TXT")
            if (drive / "RACE1.BLT").exists() or (drive / "RACE2.BLT").exists():
                raise RuntimeError("failed source-race create published an archive")
            leftovers = [path.name for path in drive.iterdir()
                         if path.name.startswith("BP") and path.name != "BOLTPACK.TOS"]
            if leftovers:
                raise RuntimeError("failed source-race create left archive stages: " +
                                   repr(leftovers))
        finally:
            if process.poll() is None:
                (drive / "CONTINUE.TXT").write_text("continue\n", encoding="ascii")
                (drive / "CONT2.TXT").write_text("continue\n", encoding="ascii")
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    parked_after_open = drive / "ORIG2.TXT"
    if parked_after_open.read_bytes() != ORIGINAL:
        raise RuntimeError("source swap fixture did not preserve the original input")
    os.replace(parked_after_open, source)
    if source.read_bytes() != ORIGINAL:
        raise RuntimeError("original source was not restored after the fixture")

    status = subprocess.run(["git", "status", "--porcelain=v1",
                             "--untracked-files=all"], cwd=ROOT,
                            stdout=subprocess.PIPE, check=True).stdout
    diff = subprocess.run(["git", "diff", "--binary", "HEAD"], cwd=ROOT,
                          stdout=subprocess.PIPE, check=True).stdout
    manifest = {
        "result": "PASS",
        "purpose": "Atari CLI rejects a changed source between pathname stat and open",
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                             cwd=ROOT, text=True).strip(),
        "worktree_status_sha256": hashlib.sha256(status).hexdigest(),
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "tools": {"hatari": command_version([args.hatari, "--version"]),
                  "atari_compiler": command_version([args.cc, "--version"])},
        "runtime": {"tos_sha256": sha256(tos),
                    "atari_cli_sha256": sha256(atari_cli),
                    "guest_runner_source_sha256": sha256(guest),
                    "stat_open_swap_changed_size":
                        before_stat_swap.st_size != after_stat_swap.st_size,
                    "stat_open_swap_changed_mtime":
                        before_stat_swap.st_mtime != after_stat_swap.st_mtime,
                    "open_read_swap_changed_size":
                        before_open_swap.st_size != after_open_swap.st_size,
                    "open_read_swap_changed_mtime":
                        before_open_swap.st_mtime != after_open_swap.st_mtime,
                    "guest_statuses": result,
                    "expected_diagnostics": ["source changed while opening",
                                             "source changed while reading"],
                    "archive_published": False,
                    "stage_files_left": leftovers,
                    "hatari_exit": process.returncode},
        "artifacts": artifact_index(output),
    }
    (output / "run.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                     encoding="utf-8")
    print("PASS: Atari source substitution refused before archive publication; evidence:",
          output / "run.json")


if __name__ == "__main__":
    main()
