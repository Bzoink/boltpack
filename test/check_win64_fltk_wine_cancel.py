#!/usr/bin/env python3
"""Cancel production Win64 FLTK extraction under Wine and verify rollback."""
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
MANIFEST = ROOT / "test/reference/interoperability/win64-fltk-wine-cancel.json"


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
        ["git", "status", "--short", "--untracked-files=all"], cwd=ROOT,
        check=True, stdout=subprocess.PIPE, text=True).stdout
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


def wait_for_title(process, fragment, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process.poll() is None:
        for window_id, title in visible_windows():
            if fragment.lower() in title.lower():
                return window_id, title
        time.sleep(0.05)
    return None


def write_payload(path, size, chunk):
    digest = hashlib.sha256()
    block = chunk.encode("utf-8")
    left = size
    with path.open("wb") as stream:
        while left:
            part = block[:min(left, len(block))]
            stream.write(part)
            digest.update(part)
            left -= len(part)
    return digest.hexdigest()


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
    parser.add_argument("--scenario", choices=("cancel", "publish-failure"),
                        default="cancel")
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
    member = manifest["member_prefix"] + run_id + manifest["member_suffix"]
    out = Path(tempfile.mkdtemp(prefix="r08-win64-fltk-cancel-",
                                dir=args.output_root.resolve()))
    source_dir = out / "source"
    source_dir.mkdir()
    source = source_dir / member
    archive = out / "linux-created.blt"
    payload_hash = write_payload(source, manifest["payload_size"],
                                 manifest["payload_chunk_utf8"])
    if payload_hash != manifest["payload_sha256"]:
        raise RuntimeError("fixture payload differs from its independent manifest")

    destination = (prefix / "drive_c" / "users" / args.wine_user / "Desktop" /
                   member)
    sentinel = manifest["destination_sentinel_utf8"].encode("utf-8")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise RuntimeError("unique Wine Desktop destination already exists")
    if args.scenario == "cancel":
        sentinel_path = destination
        sentinel_path.write_bytes(sentinel)
    else:
        destination.mkdir()
        sentinel_path = destination / manifest["directory_sentinel_name"]
        sentinel_path.write_bytes(sentinel)

    create = subprocess.run(
        [str(linux), "c", "--store", "linux-created.blt", member],
        cwd=source_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False)
    (out / "linux-create.log").write_text(create.stdout, encoding="utf-8")
    if create.returncode != 0:
        raise RuntimeError("Linux could not create the independent fixture: " + str(out))
    archive = source_dir / "linux-created.blt"
    verify = subprocess.run(
        [str(linux), "t", str(archive)], cwd=out,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        check=False)
    (out / "linux-integrity.log").write_text(verify.stdout, encoding="utf-8")
    if verify.returncode != 0:
        raise RuntimeError("Linux rejected the input archive: " + str(out))

    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix)
    env["WINEDEBUG"] = "-all"
    command = [args.wine, str(gui), "--shell-extract", str(archive)]
    process = subprocess.Popen(command, cwd=out, env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    result = "failed"
    error = None
    stage_observed = None
    stage_peak_bytes = 0
    progress_title = None
    main_window = None
    windows_after_cancel = []
    try:
        main_window = wait_for_title(process, "BoltPack")
        if not main_window:
            raise RuntimeError("main FLTK window did not appear")
        if not wait_for_title(process, "Browse for Folder"):
            raise RuntimeError("Wine folder chooser did not appear")
        subprocess.run(["xdotool", "key", "Return"], check=True)
        progress = wait_for_title(process, "Extracting...", timeout=30)
        if not progress:
            raise RuntimeError("production extraction progress window did not appear")
        progress_title = progress[1]

        deadline = time.monotonic() + 20
        stages = []
        while time.monotonic() < deadline and process.poll() is None:
            stages = list(destination.parent.glob(".boltpack-*.tmp"))
            if stages:
                stage_observed = stages[0].name
                stage_peak_bytes = max(stage_peak_bytes,
                                       max((p.stat().st_size for p in stages), default=0))
                if stage_peak_bytes > 0:
                    break
            time.sleep(0.01)
        if not stage_observed or stage_peak_bytes <= 0:
            raise RuntimeError("no partially written extraction stage was observed")

        if args.scenario == "cancel":
            subprocess.run(["xdotool", "mousemove", "--window", progress[0],
                            "200", "137", "click", "1"], check=True)
        deadline = time.monotonic() + (60 if args.scenario == "publish-failure" else 10)
        progress_closed = False
        remaining_stages = []
        while time.monotonic() < deadline and process.poll() is None:
            windows_after_cancel = visible_windows()
            progress_closed = not any(item[0] == progress[0]
                                      for item in windows_after_cancel)
            remaining_stages = list(destination.parent.glob(".boltpack-*.tmp"))
            if progress_closed and not remaining_stages:
                break
            time.sleep(0.05)
        if not progress_closed or remaining_stages:
            raise RuntimeError("operation did not close progress and remove the stage")
        remaining_stages = list(destination.parent.glob(".boltpack-*.tmp"))
        if sentinel_path.read_bytes() != sentinel:
            raise RuntimeError("operation changed the pre-existing sentinel")
        if args.scenario == "publish-failure" and not destination.is_dir():
            raise RuntimeError("failed publication replaced the existing directory")
        if remaining_stages:
            raise RuntimeError("cancellation left an extraction stage: " +
                               ", ".join(path.name for path in remaining_stages))
        if process.poll() is not None:
            raise RuntimeError("GUI exited before cancellation could be acknowledged")
        result = ("passed_cancel_preserved_destination" if args.scenario == "cancel"
                  else "passed_publish_failure_preserved_destination")
    except Exception as exc:
        error = str(exc)
    finally:
        if process.poll() is None:
            subprocess.run(["xdotool", "key", "Return"], check=False)
            time.sleep(0.1)
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
    artifacts = {str(path.relative_to(out)): sha256(path)
                 for path in sorted(out.rglob("*")) if path.is_file()}
    record = {
        "purpose": manifest["case"], "scenario": args.scenario,
        "result": result, "error": error,
        "source_revision": revision, "source_status": status,
        "source_diff_sha256": diff_sha,
        "runtime": {"wine": wine_version, "display": os.environ["DISPLAY"]},
        "manifest": str(MANIFEST.relative_to(ROOT)),
        "manifest_sha256": sha256(MANIFEST),
        "runner": str(Path(__file__).relative_to(ROOT)),
        "runner_sha256": sha256(Path(__file__)),
        "command": command, "gui_sha256": sha256(gui),
        "linux_cli_sha256": sha256(linux),
        "archive_sha256": sha256(archive), "archive_size": archive.stat().st_size,
        "member": member, "source_sha256": sha256(source),
        "source_size": source.stat().st_size,
        "destination": str(destination),
        "destination_kind": "directory" if destination.is_dir() else "file",
        "destination_sha256": sha256(destination) if destination.is_file() else None,
        "sentinel_path": str(sentinel_path),
        "sentinel_sha256": sha256(sentinel_path) if sentinel_path.is_file() else None,
        "destination_preserved": sentinel_path.is_file() and
                                 sentinel_path.read_bytes() == sentinel and
                                 (args.scenario != "publish-failure" or
                                  destination.is_dir()),
        "stage_observed": stage_observed, "stage_peak_bytes": stage_peak_bytes,
        "progress_window_title": progress_title,
        "windows_after_cancel": windows_after_cancel,
        "remaining_stages": [path.name for path in destination.parent.glob(
            ".boltpack-*.tmp")],
        "wine_exit_status": process.returncode, "artifacts": artifacts,
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    if result == "passed_cancel_preserved_destination":
        destination.unlink()
    elif result == "passed_publish_failure_preserved_destination":
        sentinel_path.unlink()
        destination.rmdir()
    print(json.dumps(record, indent=2))
    expected_result = ("passed_cancel_preserved_destination"
                       if args.scenario == "cancel" else
                       "passed_publish_failure_preserved_destination")
    if result != expected_result or process.returncode != 0:
        raise SystemExit("Win64 FLTK Wine cancellation failed; evidence: " + str(out))


if __name__ == "__main__":
    main()
