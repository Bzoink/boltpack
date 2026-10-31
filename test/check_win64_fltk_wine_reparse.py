#!/usr/bin/env python3
"""Verify the Win64 FLTK creator refuses a source tree containing a symlink."""
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
MANIFEST = ROOT / "test/reference/interoperability/win64-fltk-wine-reparse.json"


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


def windows():
    result = subprocess.run(
        ["xdotool", "search", "--onlyvisible", "--name", "."],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        check=False)
    found = []
    for window_id in result.stdout.split():
        title = subprocess.run(
            ["xdotool", "getwindowname", window_id],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            check=False).stdout.strip()
        if title:
            found.append((window_id, title))
    return found


def wait_title(process, fragment, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process.poll() is None:
        for window_id, title in windows():
            if fragment.lower() in title.lower():
                return window_id, title
        time.sleep(0.1)
    return None


def close_gui(process):
    if process.poll() is None:
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

    gui = args.gui.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    prefix = args.wine_prefix.expanduser().resolve(strict=True)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    out = Path(tempfile.mkdtemp(prefix="r08-win64-fltk-reparse-",
                                dir=args.output_root.resolve()))
    source_dir = out / manifest["source_directory"]
    source_dir.mkdir()
    outside = out / manifest["outside_filename"]
    outside_payload = manifest["outside_payload_utf8"].encode("utf-8")
    outside.write_bytes(outside_payload)
    link = source_dir / "outside-link.txt"
    os.symlink(outside, link)
    if not link.is_symlink():
        raise RuntimeError("host did not create the symlink fixture")

    archive = out / manifest["expected_archive"]
    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix)
    env["WINEDEBUG"] = "-all"
    command = [args.wine, str(gui), "--shell-create-boltlz", str(source_dir)]
    process = subprocess.Popen(command, cwd=out, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    result = "failed"
    error = None
    save_dialog = None
    after_submit_windows = []
    try:
        main_window = wait_title(process, "BoltPack")
        save_dialog = wait_title(process, "Save archive as")
        if not main_window or not save_dialog:
            raise RuntimeError("GUI or archive save dialog did not appear")
        subprocess.run(["xdotool", "key", "alt+s"], check=True)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and process.poll() is None:
            after_submit_windows = windows()
            if not any("Save archive as" in title for _, title in after_submit_windows):
                # Saving closes the chooser; give the worker and error alert time
                # to finish before checking whether an archive was published.
                time.sleep(0.5)
                after_submit_windows = windows()
                break
            time.sleep(0.1)
        if any("Save archive as" in title for _, title in after_submit_windows):
            raise RuntimeError("save chooser did not close after submission")
        if not archive.exists() and len(after_submit_windows) < 2:
            raise RuntimeError("GUI did not show an error dialog after refusing the source")
        if not archive.exists():
            result = "refused_without_output"
    except Exception as exc:
        error = str(exc)
    finally:
        close_gui(process)

    residue = sorted(path.name for path in out.iterdir()
                     if path not in (source_dir, outside, archive))
    if residue:
        error = (error + "; " if error else "") + \
            "unexpected output/stage residue: " + repr(residue)
        result = "failed_with_residue"
    linux_test_status = None
    extract_status = None
    extracted = out / "linux-extracted"
    extracted_sha = None
    extracted_matches_outside = False
    if archive.is_file():
        linux_test = subprocess.run(
            [str(linux), "t", str(archive)], cwd=out,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            check=False)
        (out / "linux-test.log").write_text(linux_test.stdout, encoding="utf-8")
        linux_test_status = linux_test.returncode
        extracted.mkdir()
        extraction = subprocess.run(
            [str(linux), "x", "-o", str(extracted), str(archive)], cwd=out,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            check=False)
        (out / "linux-extract.log").write_text(extraction.stdout, encoding="utf-8")
        extract_status = extraction.returncode
        output_member = extracted / manifest["source_directory"] / link.name
        if output_member.is_file():
            extracted_sha = sha256(output_member)
            extracted_matches_outside = extracted_sha == sha256(outside)
        if extracted_matches_outside:
            result = "failed_symlink_target_archived"
            error = "Wine exposed the host symlink as an ordinary file; outside bytes were archived"
        elif result != "refused_without_output":
            result = "failed_archive_contents_unexpected"
    version = subprocess.run(
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
        "runtime": {"wine": version, "display": os.environ["DISPLAY"]},
        "manifest": str(MANIFEST.relative_to(ROOT)),
        "manifest_sha256": sha256(MANIFEST),
        "runner": str(Path(__file__).relative_to(ROOT)),
        "runner_sha256": sha256(Path(__file__)),
        "command": command,
        "gui_sha256": sha256(gui),
        "linux_cli_sha256": sha256(linux),
        "source_directory": str(source_dir.relative_to(ROOT)),
        "symlink_target": str(outside.relative_to(ROOT)),
        "symlink_target_sha256": sha256(outside),
        "host_symlink_created": link.is_symlink(),
        "save_dialog": save_dialog,
        "windows_after_submit": after_submit_windows,
        "archive_published": archive.exists(),
        "archive_sha256": sha256(archive) if archive.is_file() else None,
        "linux_test_status": linux_test_status,
        "linux_extraction_status": extract_status,
        "extracted_member_sha256": extracted_sha,
        "extracted_member_matches_outside_target": extracted_matches_outside,
        "output_directory_entries": sorted(path.name for path in out.iterdir()),
        "wine_exit_status": process.returncode,
        "source_tree": {
            "link_name": link.name,
            "link_text": os.readlink(link),
            "outside_payload_size": len(outside_payload),
        },
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print(json.dumps(record, indent=2))
    if result != "refused_without_output" or process.returncode != 0:
        raise SystemExit("Win64 FLTK reparse refusal failed; evidence: " + str(out))


if __name__ == "__main__":
    main()
