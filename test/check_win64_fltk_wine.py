#!/usr/bin/env python3
"""Exercise Win64 FLTK Unicode extraction through Wine's folder picker."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import getpass
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "test/reference/interoperability/win64-fltk-wine.json"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_snapshot():
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
        stdout=subprocess.PIPE, text=True).stdout.strip()
    status = subprocess.run(
        ["git", "status", "--short"], cwd=ROOT, check=True,
        stdout=subprocess.PIPE, text=True).stdout
    diff = subprocess.run(
        ["git", "diff", "--binary", "HEAD"], cwd=ROOT, check=True,
        stdout=subprocess.PIPE).stdout
    return revision, status, hashlib.sha256(diff).hexdigest()


def visible_windows():
    result = subprocess.run(
        ["xdotool", "search", "--onlyvisible", "--name", "."],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        check=False)
    windows = []
    for window_id in result.stdout.split():
        title = subprocess.run(
            ["xdotool", "getwindowname", window_id],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            check=False).stdout.strip()
        if title:
            windows.append((window_id, title))
    return windows


def wait_for_title(process, fragment, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process.poll() is None:
        for window_id, title in visible_windows():
            if fragment.lower() in title.lower():
                return window_id, title
        time.sleep(0.1)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", default="/usr/lib/wine/wine64")
    parser.add_argument("--wine-prefix", type=Path,
                        default=ROOT / "build/win64/wineprefix-interop64")
    parser.add_argument("--wine-user", default=getpass.getuser())
    parser.add_argument("--gui", type=Path,
                        default=ROOT / "build/win64-fltk-gui/boltpack-gui.exe")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    if not os.environ.get("DISPLAY"):
        parser.error("run under Xvfb or a desktop display")
    if subprocess.run(["xdotool", "-v"], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL, check=False).returncode != 0:
        parser.error("xdotool is required")

    gui = args.gui.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    prefix = args.wine_prefix.expanduser().resolve(strict=True)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    run_id = uuid.uuid4().hex[:10]
    member = (manifest["member_prefix"] + run_id +
              manifest["member_suffix"])
    out = Path(tempfile.mkdtemp(prefix="r08-win64-fltk-wine-",
                                dir=args.output_root.resolve()))
    source_dir = out / "source"
    source_dir.mkdir()
    source = source_dir / member
    archive = out / "linux-created.blt"
    source.write_text(manifest["payload_utf8"], encoding="utf-8", newline="")
    expected = source.read_bytes()
    if sha256(source) != manifest["payload_sha256"]:
        raise RuntimeError("fixture payload differs from its independent manifest")

    create = subprocess.run(
        [str(linux), "c", "--store", str(archive), member],
        cwd=source_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False)
    (out / "linux-create.log").write_text(create.stdout, encoding="utf-8")
    if create.returncode != 0:
        raise RuntimeError("Linux could not create the independent fixture: " +
                           str(out))
    verify = subprocess.run(
        [str(linux), "t", str(archive)], cwd=out,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        check=False)
    (out / "linux-integrity.log").write_text(verify.stdout, encoding="utf-8")
    if verify.returncode != 0:
        raise RuntimeError("Linux rejected the input archive: " + str(out))

    destination = (prefix / "drive_c" / "users" / args.wine_user / "Desktop" /
                   member)
    if destination.exists():
        raise RuntimeError("unique Wine Desktop destination already exists")
    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix)
    env["WINEDEBUG"] = "-all"
    command = [args.wine, str(gui), "--shell-extract", str(archive)]
    process = subprocess.Popen(command, cwd=out, env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    result = "failed"
    error = None
    try:
        main_window = wait_for_title(process, "BoltPack")
        chooser = wait_for_title(process, "Browse for Folder")
        if not main_window or not chooser:
            raise RuntimeError("GUI or folder chooser did not appear")
        # Wine's Shell folder dialog starts with Desktop selected.
        subprocess.run(["xdotool", "key", "Return"], check=True)
        deadline = time.monotonic() + 20
        while (time.monotonic() < deadline and process.poll() is None and
               not destination.is_file()):
            time.sleep(0.1)
        if not destination.is_file():
            raise RuntimeError("GUI did not extract into the selected Desktop")
        if destination.read_bytes() != expected:
            raise RuntimeError("Win64 FLTK output differs from the source bytes")
        result = "passed_under_wine"
    except Exception as exc:  # retain a failure record with runtime state
        error = str(exc)
    finally:
        if process.poll() is None:
            # Dismiss the completion/error dialog if present, then close via menu.
            subprocess.run(["xdotool", "key", "Return"], check=False)
            time.sleep(0.2)
            subprocess.run(["xdotool", "key", "ctrl+q"], check=False)
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    wine_version = subprocess.run(
        [args.wine, "--version"], env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, check=False).stdout.strip()
    revision, status, diff_sha = source_snapshot()
    record = {
        "purpose": manifest["case"],
        "result": result,
        "error": error,
        "source_revision": revision,
        "source_status": status,
        "source_diff_sha256": diff_sha,
        "runtime": {"wine": wine_version, "display": os.environ["DISPLAY"]},
        "manifest": str(MANIFEST.relative_to(ROOT)),
        "manifest_sha256": sha256(MANIFEST),
        "runner": str(Path(__file__).relative_to(ROOT)),
        "runner_sha256": sha256(Path(__file__)),
        "command": command,
        "gui_sha256": sha256(gui),
        "linux_cli_sha256": sha256(linux),
        "archive": str(archive.relative_to(ROOT)),
        "archive_sha256": sha256(archive),
        "member": member,
        "source_sha256": sha256(source),
        "destination": str(destination),
        "destination_sha256": sha256(destination) if destination.is_file() else None,
        "payload_byte_identical": destination.is_file() and
                                  destination.read_bytes() == expected,
        "wine_exit_status": process.returncode,
        "artifacts": {path.name: sha256(path) for path in out.iterdir()
                      if path.is_file()},
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print(json.dumps(record, indent=2))
    if result != "passed_under_wine" or process.returncode != 0:
        raise SystemExit("Win64 FLTK Wine extraction failed; evidence: " + str(out))


if __name__ == "__main__":
    main()
