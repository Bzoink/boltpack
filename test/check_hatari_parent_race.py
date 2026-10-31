#!/usr/bin/env python3
"""Check Atari CLI extraction refuses output-root substitution on Hatari."""
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
PAYLOAD = b"Atari CLI parent substitution payload\r\n" * 64
SENTINEL = b"redirected destination must remain unchanged\r\n"


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


def wait_for(path, process, timeout, nonempty=False):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and (not nonempty or path.stat().st_size > 0):
            return
        if process.poll() is not None:
            raise RuntimeError("Hatari exited before marker: " + path.name)
        time.sleep(0.05)
    raise RuntimeError("timed out waiting for Hatari marker: " + path.name)


def wait_for_lines(path, process, timeout, count):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and len(path.read_text(encoding="ascii").splitlines()) >= count:
            return
        if process.poll() is not None:
            raise RuntimeError("Hatari exited before guest result line " + str(count))
        time.sleep(0.05)
    raise RuntimeError("timed out waiting for guest result line " + str(count))


def replace_root_with_redirect(root, moved, redirect):
    os.replace(root, moved)
    root.symlink_to(redirect, target_is_directory=True)


def restore_root(root, moved):
    if root.is_symlink():
        root.unlink()
    if moved.exists():
        os.replace(moved, root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", required=True, type=Path)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--cc", default="m68k-atari-mint-gcc")
    parser.add_argument("--linux-cli", type=Path, required=True)
    parser.add_argument("--atari-cli", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(
        tempfile.mkdtemp(prefix="atari-parent-race-", dir=ROOT / "build"))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    drive = output / "drive"
    drive.mkdir()
    tos = args.tos.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    atari = args.atari_cli.resolve(strict=True)
    shutil.copyfile(atari, drive / "BOLTPACK.TOS")
    runner = drive / "RUN.TOS"
    guest_source = ROOT / "test/atari/guest_runner.c"
    compiled = subprocess.run([args.cc, "-O2", str(guest_source), "-o", str(runner)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              check=False)
    (output / "compile.log").write_bytes(compiled.stdout)
    if compiled.returncode != 0:
        raise RuntimeError("guest runner compilation failed; see " + str(output))

    (drive / "PAYLOAD.TXT").write_bytes(PAYLOAD)
    archive = drive / "INPUT.BLT"
    subprocess.run([str(linux), "c", str(archive), "PAYLOAD.TXT"],
                   cwd=drive, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                   check=True)
    roots = [drive / "OUT1", drive / "OUT2"]
    redirects = [drive / "REDIR1", drive / "REDIR2"]
    moved = [drive / "MOVED1", drive / "MOVED2"]
    for root in roots:
        root.mkdir()
    for redirect in redirects:
        redirect.mkdir()
        (redirect / "PAYLOAD.TXT").write_bytes(SENTINEL)
        os.utime(redirect, (1600000000, 1600000000))
    (drive / "COMMANDS.TXT").write_text(
        "x -f -o OUT1 INPUT.BLT\nx -f -o OUT2 INPUT.BLT\n", encoding="ascii")

    log_path = output / "hatari.log"
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    command = [args.hatari, "--machine", "ste", "--tos", str(tos),
               "--sound", "off", "--confirm-quit", "off", "--fast-forward", "on",
               "--fast-boot", "on", "--memsize", "4", "--harddrive", str(drive),
               "--auto", r"C:\RUN.TOS", "--run-vbls", "16000"]
    with log_path.open("wb") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(drive / "ROOTCHK.TXT", process, 90)
            replace_root_with_redirect(roots[0], moved[0], redirects[0])
            (drive / "CNTROOT.TXT").write_text("continue\n", encoding="ascii")
            wait_for_lines(drive / "RESULT.TXT", process, 90, 1)
            restore_root(roots[0], moved[0])

            wait_for(drive / "STAGEOK.TXT", process, 90)
            staged_before = sorted(path.name for path in roots[1].iterdir())
            replace_root_with_redirect(roots[1], moved[1], redirects[1])
            (drive / "CNTSTG.TXT").write_text("continue\n", encoding="ascii")
            wait_for_lines(drive / "RESULT.TXT", process, 90, 2)
            results = (drive / "RESULT.TXT").read_text(encoding="ascii").splitlines()
            if results != ["1", "1"]:
                raise RuntimeError("substituted output roots were not refused: " + repr(results))
            if (redirects[0] / "PAYLOAD.TXT").read_bytes() != SENTINEL or \
                    (redirects[1] / "PAYLOAD.TXT").read_bytes() != SENTINEL:
                raise RuntimeError("a redirected destination sentinel was overwritten")
            redirected_stages = [path.name for redirect in redirects
                                 for path in redirect.iterdir()
                                 if path.name.startswith("BP")]
            if redirected_stages:
                raise RuntimeError("a stage was created in a redirected root: " +
                                   repr(redirected_stages))
            if sorted(path.name for path in roots[0].iterdir()) != []:
                raise RuntimeError("pre-stage substitution unexpectedly created output")
            stages = [path for path in moved[1].iterdir() if path.name.startswith("BP")]
            if len(stages) != 1 or stages[0].read_bytes() != PAYLOAD:
                raise RuntimeError("post-stage substitution did not retain exactly the verified stage")
            stage_record = {"name": stages[0].name,
                            "size": stages[0].stat().st_size,
                            "sha256": sha256(stages[0])}
            if (drive / "LOG00.TXT").read_text(errors="replace").find(
                    "output root changed") < 0 or \
                    (drive / "LOG01.TXT").read_text(errors="replace").find(
                    "output root changed") < 0:
                raise RuntimeError("Atari CLI did not report both root substitutions")
        finally:
            (drive / "CNTROOT.TXT").write_text("continue\n", encoding="ascii")
            (drive / "CNTSTG.TXT").write_text("continue\n", encoding="ascii")
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    restore_root(roots[0], moved[0])
    restore_root(roots[1], moved[1])

    status = subprocess.run(["git", "status", "--porcelain=v1",
                             "--untracked-files=all"], cwd=ROOT,
                            stdout=subprocess.PIPE, check=True).stdout
    diff = subprocess.run(["git", "diff", "--binary", "HEAD"], cwd=ROOT,
                          stdout=subprocess.PIPE, check=True).stdout
    manifest = {
        "result": "PASS",
        "purpose": "Atari CLI refuses output-root substitution before staging and before publication",
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                             cwd=ROOT, text=True).strip(),
        "worktree_status_sha256": hashlib.sha256(status).hexdigest(),
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "source_sha256": {name: sha256(ROOT / name) for name in (
            "Makefile", "src/frontend/atari/main.c",
            "test/check_hatari_parent_race.py", "test/atari/guest_runner.c")},
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "tools": {"hatari": command_version([args.hatari, "--version"]),
                  "atari_compiler": command_version([args.cc, "--version"])},
        "inputs": {"tos_sha256": sha256(tos), "linux_cli_sha256": sha256(linux),
                   "atari_cli_sha256": sha256(atari),
                   "payload_sha256": hashlib.sha256(PAYLOAD).hexdigest(),
                   "redirect_sentinel_sha256": hashlib.sha256(SENTINEL).hexdigest()},
        "observed": {"guest_statuses": results,
                     "pre_stage_substitution_refused": True,
                     "post_stage_substitution_refused": True,
                     "redirect_sentinels_preserved": True,
                     "verified_stage_left_in_moved_original_parent":
                         stage_record,
                     "stages_present_before_second_substitution": staged_before,
                     "redirected_publication": False,
                     "hatari_exit": process.returncode},
        "artifacts": artifact_index(output),
    }
    (output / "run.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                     encoding="utf-8")
    print("PASS: Atari CLI refused output-root substitution and preserved redirected sentinels; evidence:",
          output / "run.json")


if __name__ == "__main__":
    main()
