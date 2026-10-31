#!/usr/bin/env python3
"""Test Win64 FLTK recursive UTF-8 source enumeration and archive creation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import getpass
import re
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "test/reference/interoperability/win64-fltk-wine-create.json"


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


def run_command(command, cwd, log_path):
    result = subprocess.run(
        [str(item) for item in command], cwd=cwd,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        check=False)
    log_path.write_text(result.stdout, encoding="utf-8")
    return result.returncode


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
    parser.add_argument("--mini-cli", type=Path,
                        default=ROOT / "build/linux32mini/bpmini")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    parser.add_argument("--profile1", action="store_true",
                        help="enable Interchange Profile 1 in the FLTK preferences")
    args = parser.parse_args()
    if not os.environ.get("DISPLAY"):
        parser.error("run under Xvfb or a desktop display")

    gui = args.gui.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    mini = args.mini_cli.resolve(strict=True)
    prefix = args.wine_prefix.expanduser().resolve(strict=True)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    run_id = uuid.uuid4().hex[:10]
    member = manifest["member_prefix"] + run_id + manifest["member_suffix"]
    out = Path(tempfile.mkdtemp(prefix="r08-win64-fltk-wine-create-",
                                dir=args.output_root.resolve()))
    source_dir = out / manifest["source_directory"]
    source_dir.mkdir()
    source = source_dir / member
    archive = out / (manifest["source_directory"] + ".blt")
    extracted = out / "linux-extracted"
    payload = (manifest["payload_line_utf8"].encode("utf-8") *
               int(manifest["payload_repetitions"]))
    source.write_bytes(payload)
    if (len(payload) != manifest["payload_size"] or
            hashlib.sha256(payload).hexdigest() != manifest["payload_sha256"]):
        raise RuntimeError("source payload differs from the independent manifest")

    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix)
    env["WINEDEBUG"] = "-all"
    prefs_path = (prefix / "drive_c" / "users" / args.wine_user /
                  "AppData" / "Roaming" / "BoltPack" / "gui.conf")
    previous_prefs = None
    if args.profile1:
        previous_prefs = prefs_path.read_bytes() if prefs_path.exists() else None
        prefs_path.parent.mkdir(parents=True, exist_ok=True)
        lines = (previous_prefs.decode("utf-8").splitlines()
                 if previous_prefs is not None else [])
        lines = [line for line in lines
                 if not re.match(r"^\s*interchange_profile=", line)]
        lines.append("interchange_profile=1")
        prefs_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    command = [args.wine, str(gui), "--shell-create-boltlz", str(source_dir)]
    process = subprocess.Popen(command, cwd=out, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    result = "failed"
    error = None
    main_window = None
    save_dialog = None
    try:
        main_window = wait_for_title(process, "BoltPack")
        save_dialog = wait_for_title(process, "Save archive as")
        if not main_window or not save_dialog:
            raise RuntimeError("GUI or archive save dialog did not appear")
        # The source directory path is ASCII; its Unicode child name is read
        # by the production recursive enumeration path.
        subprocess.run(["xdotool", "key", "alt+s"], check=True)
        deadline = time.monotonic() + 30
        while (time.monotonic() < deadline and process.poll() is None and
               not archive.is_file()):
            time.sleep(0.1)
        if not archive.is_file():
            raise RuntimeError("GUI did not publish the suggested source.blt archive")
        result = "created"
    except Exception as exc:
        error = str(exc)
    finally:
        if process.poll() is None:
            # Dismiss the completion/error dialog and exit through the GUI menu.
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
    if args.profile1:
        if previous_prefs is None:
            prefs_path.unlink(missing_ok=True)
        else:
            prefs_path.write_bytes(previous_prefs)

    linux_test_status = None
    mini_test_status = None
    extract_status = None
    archive_info = ""
    extracted.mkdir()
    output = extracted / manifest["source_directory"] / member
    if archive.is_file():
        linux_test_status = run_command(
            [linux, "t", archive], out, out / "linux-test.log")
        mini_test_status = run_command(
            [mini, "t", archive], out, out / "mini-test.log")
        info = subprocess.run(
            [str(linux), "i", str(archive)], cwd=out,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, check=False)
        archive_info = info.stdout
        (out / "linux-info.log").write_text(archive_info, encoding="utf-8")
        extract_status = run_command(
            [linux, "x", "-o", extracted, archive], out,
            out / "linux-extract.log")
        profile_ok = (not args.profile1 or
                      (manifest.get("profile1_expected", {}).get("method") ==
                       "BoltLite" and "Method:         BoltLite (1)" in archive_info and
                       mini_test_status == 0))
        if (linux_test_status == 0 and mini_test_status == 0 and
                extract_status == 0 and output.is_file() and
                output.read_bytes() == payload and profile_ok):
            result = "passed_under_wine"
        else:
            error = "Linux full/mini/profile check or exact-byte extraction failed"

    version = subprocess.run(
        [args.wine, "--version"], env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, check=False).stdout.strip()
    revision, status, diff_sha = source_snapshot()
    record = {
        "purpose": manifest["case"],
        "profile1_requested": args.profile1,
        "profile1_method_observed": ("BoltLite" if
                                     "Method:         BoltLite (1)" in archive_info
                                     else None),
        "archive_info": archive_info,
        "result": result,
        "error": error,
        "source_revision": revision,
        "source_status": status,
        "source_diff_sha256": diff_sha,
        "runtime": {"wine": version, "display": os.environ["DISPLAY"]},
        "manifest": str(MANIFEST.relative_to(ROOT)),
        "manifest_sha256": sha256(MANIFEST),
        "runner": str(Path(__file__).relative_to(ROOT)),
        "runner_sha256": sha256(Path(__file__)),
        "command": command,
        "gui_sha256": sha256(gui),
        "linux_cli_sha256": sha256(linux),
        "mini_cli_sha256": sha256(mini),
        "save_dialog": save_dialog,
        "archive": str(archive.relative_to(ROOT)),
        "archive_sha256": sha256(archive) if archive.is_file() else None,
        "member": "source/" + member,
        "source_size": len(payload),
        "source_sha256": sha256(source),
        "linux_test_status": linux_test_status,
        "mini_test_status": mini_test_status,
        "linux_extract_status": extract_status,
        "extracted_sha256": sha256(output) if output.is_file() else None,
        "payload_byte_identical": output.is_file() and
                                  output.read_bytes() == payload,
        "wine_exit_status": process.returncode,
        "artifacts": {path.name: sha256(path) for path in out.iterdir()
                      if path.is_file()},
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print(json.dumps(record, indent=2))
    if result != "passed_under_wine" or process.returncode != 0:
        raise SystemExit("Win64 FLTK Wine create failed; evidence: " + str(out))


if __name__ == "__main__":
    main()
