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
    parser.add_argument("--fat-attributes", action="store_true",
                        help="round-trip HIDDEN/SYSTEM/ARCHIVE through the production GUI")
    parser.add_argument("--directory-race", action="store_true",
                        help="prove a guarded source directory cannot be renamed while enumerated")
    parser.add_argument("--child-race", action="store_true",
                        help="refuse a source child replaced after enumeration")
    parser.add_argument("--attributes-probe", type=Path,
                        default=ROOT / "build/win64-fltk-gui/attributes-probe.exe")
    args = parser.parse_args()
    if not os.environ.get("DISPLAY"):
        parser.error("run under Xvfb or a desktop display")

    gui = args.gui.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    mini = args.mini_cli.resolve(strict=True)
    prefix = args.wine_prefix.expanduser().resolve(strict=True)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if args.directory_race and args.child_race:
        parser.error("select only one source race case")
    if args.directory_race or args.child_race:
        expected_key = ("directory_race_expected" if args.directory_race
                        else "child_race_expected")
        expected_value = ("refuse substitution before archive publication"
                          if args.directory_race else
                          "refuse same-metadata substitution before archive publication")
        if manifest.get(expected_key) != expected_value:
            parser.error("manifest does not define the expected source-race outcome")
    attrs_probe = (args.attributes_probe.resolve(strict=True)
                   if args.fat_attributes or args.directory_race or args.child_race
                   else None)
    run_id = uuid.uuid4().hex[:10]
    member = manifest["member_prefix"] + run_id + manifest["member_suffix"]
    out = Path(tempfile.mkdtemp(prefix="r08-win64-fltk-wine-create-",
                                dir=args.output_root.resolve()))
    source_dir = out / manifest["source_directory"]
    source_dir.mkdir()
    source = source_dir / member
    source_for_hash = source
    archive = out / (manifest["source_directory"] + ".blt")
    extracted = out / "linux-extracted"
    payload = (manifest["payload_line_utf8"].encode("utf-8") *
               int(manifest["payload_repetitions"]))
    source.write_bytes(payload)
    if (len(payload) != manifest["payload_size"] or
            hashlib.sha256(payload).hexdigest() != manifest["payload_sha256"]):
        raise RuntimeError("source payload differs from the independent manifest")

    attr_set_results = []
    if args.fat_attributes:
        for subject in (source_dir, source):
            attr_set = subprocess.run(
                [args.wine, str(attrs_probe), "set", str(subject)],
                env={**os.environ, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all"},
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, check=False)
            attr_set_results.append({"path": str(subject), "status": attr_set.returncode,
                                     "output": attr_set.stdout.strip()})
            if attr_set.returncode != 0:
                raise RuntimeError("could not set source Windows attributes: " +
                                   attr_set.stdout)

    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix)
    env["WINEDEBUG"] = "-all"
    if args.directory_race:
        env["BP_TEST_WIN_FLTK_RACE_PHASE"] = "directory"
    elif args.child_race:
        env["BP_TEST_WIN_FLTK_RACE_PHASE"] = "child"
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
    attribute_roundtrip = None
    source_race_move = None
    child_race_metadata_copy = None
    child_race_metadata_check = None
    try:
        main_window = wait_for_title(process, "BoltPack")
        save_dialog = wait_for_title(process, "Save archive as")
        if not main_window or not save_dialog:
            raise RuntimeError("GUI or archive save dialog did not appear")
        # The source directory path is ASCII; its Unicode child name is read
        # by the production recursive enumeration path.
        subprocess.run(["xdotool", "key", "alt+s"], check=True)
        if args.directory_race or args.child_race:
            if args.directory_race:
                ready = out / "source-guard.ready"
                release = out / "source-guard.release"
                parked = out / "source-parked"
                move_source = source_dir
            else:
                ready = out / "child-enumerated.ready"
                release = out / "child-enumerated.release"
                parked = out / "child-parked.txt"
                move_source = source
            deadline = time.monotonic() + 15
            while (time.monotonic() < deadline and process.poll() is None and
                   not ready.exists()):
                time.sleep(0.02)
            if not ready.exists():
                raise RuntimeError("production source-race checkpoint was not reached")
            move = subprocess.run(
                [args.wine, str(attrs_probe), "move", str(move_source), str(parked)],
                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, check=False)
            source_race_move = {"status": move.returncode,
                                "output": move.stdout.strip()}
            if move.returncode == 0:
                # Wine's hostfs mapping may permit a host-visible rename even
                # while a Win32 handle is open. Leave a counterfeit at the
                # selected pathname so the production identity check must refuse.
                if args.directory_race:
                    source_dir.mkdir()
                    source.write_bytes(b"substituted source directory\n")
                    source_for_hash = parked / member
                else:
                    replacement_payload = bytes(value ^ 0xff for value in payload)
                    source.write_bytes(replacement_payload)
                    source_for_hash = parked
                    copy_times = subprocess.run(
                        [args.wine, str(attrs_probe), "copytimes",
                         str(parked), str(source)], env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, check=False)
                    child_race_metadata_copy = {
                        "status": copy_times.returncode,
                        "output": copy_times.stdout.strip()}
                    if copy_times.returncode != 0:
                        raise RuntimeError("could not clone source metadata for race fixture: " +
                                           copy_times.stdout.strip())
                    compared = subprocess.run(
                        [args.wine, str(attrs_probe), "same-metadata",
                         str(parked), str(source)], env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, check=False)
                    child_race_metadata_check = {
                        "status": compared.returncode,
                        "output": compared.stdout.strip()}
                    if compared.returncode != 0:
                        raise RuntimeError("same-metadata child fixture was not exact: " +
                                           compared.stdout.strip())
            elif move.returncode != 1 or "error=32" not in move.stdout:
                raise RuntimeError("source rename failed unexpectedly: " +
                                   move.stdout.strip())
            release.write_text("continue\n", encoding="ascii")
            if move.returncode == 0:
                time.sleep(0.5)
                if archive.exists():
                    raise RuntimeError("GUI published an archive after source substitution")
                stages = list(out.glob(".boltpack-create-*.tmp"))
                if stages:
                    raise RuntimeError("GUI left archive stages after refusing source substitution")
                subprocess.run(["xdotool", "key", "Return"], check=False)
                time.sleep(0.2)
                subprocess.run(["xdotool", "key", "ctrl+q"], check=False)
                process.wait(timeout=8)
                if args.directory_race:
                    fixture_ok = (parked.is_dir() and source.is_file() and
                                  source.read_bytes() ==
                                  b"substituted source directory\n" and
                                  (parked / member).read_bytes() == payload)
                else:
                    fixture_ok = (parked.is_file() and source.is_file() and
                                  len(source.read_bytes()) == len(payload) and
                                  source.read_bytes() != payload and
                                  parked.read_bytes() == payload)
                if not fixture_ok:
                    raise RuntimeError("source-race fixture changed a source artifact")
                result = "passed_under_wine_refused_substitution"
            else:
                deadline = time.monotonic() + 30
                while (time.monotonic() < deadline and process.poll() is None and
                       not archive.is_file()):
                    time.sleep(0.1)
                if not archive.is_file():
                    raise RuntimeError("GUI did not publish after Win32 rename refusal")
                result = "created"
        else:
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
        if args.directory_race or args.child_race:
            release_name = ("source-guard.release" if args.directory_race
                            else "child-enumerated.release")
            (out / release_name).write_text("continue\n", encoding="ascii")
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
    method_line = ""
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
        method_line = next((line for line in archive_info.splitlines()
                            if line.startswith("Method:")), "")
        (out / "linux-info.log").write_text(archive_info, encoding="utf-8")
        extract_status = run_command(
            [linux, "x", "-o", extracted, archive], out,
            out / "linux-extract.log")
        profile_ok = (not args.profile1 or
                      (manifest.get("profile1_expected", {}).get("method") ==
                       "BoltLite" and "BoltLite" in method_line and
                       "boltlz" not in method_line.lower() and
                       mini_test_status == 0))
        extraction_status_ok = (extract_status in (0, 1)
                                if args.fat_attributes else extract_status == 0)
        if (linux_test_status == 0 and mini_test_status == 0 and
                extraction_status_ok and output.is_file() and
                output.read_bytes() == payload and profile_ok):
            result = "passed_under_wine"
        else:
            error = "Linux full/mini/profile check or exact-byte extraction failed"

    extraction_command = None
    extraction_exit_status = None
    if result == "passed_under_wine" and args.fat_attributes:
        destination = (prefix / "drive_c" / "users" / args.wine_user /
                       "Desktop" / manifest["source_directory"] / member)
        destination_dir = destination.parent
        extraction_command = [args.wine, str(gui), "--shell-extract", str(archive)]
        extract_process = subprocess.Popen(
            extraction_command, cwd=out, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            start_new_session=True)
        try:
            chooser = wait_for_title(extract_process, "Browse for Folder")
            if not chooser:
                raise RuntimeError("GUI extraction folder chooser did not appear")
            subprocess.run(["xdotool", "key", "Return"], check=True)
            deadline = time.monotonic() + 30
            while (time.monotonic() < deadline and
                   extract_process.poll() is None and not destination.is_file()):
                time.sleep(0.05)
            if not destination.is_file() or destination.read_bytes() != payload:
                raise RuntimeError("GUI did not extract the exact attributed payload")
            attr_results = []
            for subject in (destination_dir, destination):
                checked = subprocess.run(
                    [args.wine, str(attrs_probe), "check", str(subject)],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, check=False)
                attr_results.append({"path": str(subject),
                                     "status": checked.returncode,
                                     "output": checked.stdout.strip()})
            if any(item["status"] != 0 for item in attr_results):
                raise RuntimeError("GUI did not restore all FAT attributes: " +
                                   repr(attr_results))
            attribute_roundtrip = attr_results
        except Exception as exc:
            result = "failed"
            error = str(exc)
        finally:
            if extract_process.poll() is None:
                subprocess.run(["xdotool", "key", "Return"], check=False)
                time.sleep(0.1)
                subprocess.run(["xdotool", "key", "ctrl+q"], check=False)
                try:
                    extract_process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    extract_process.terminate()
                    try:
                        extract_process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        extract_process.kill()
                        extract_process.wait()
        extraction_exit_status = extract_process.returncode

    version = subprocess.run(
        [args.wine, "--version"], env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, check=False).stdout.strip()
    revision, status, diff_sha = source_snapshot()
    record = {
        "purpose": manifest["case"],
        "profile1_requested": args.profile1,
        "fat_attributes_requested": args.fat_attributes,
        "directory_race_requested": args.directory_race,
        "child_race_requested": args.child_race,
        "source_race_move": source_race_move,
        "child_race_metadata_copy": child_race_metadata_copy,
        "child_race_metadata_check": child_race_metadata_check,
        "directory_race_expected": manifest.get("directory_race_expected"),
        "child_race_expected": manifest.get("child_race_expected"),
        "directory_race_parked_source_sha256": (
            sha256(out / "source-parked" / member)
            if args.directory_race and (out / "source-parked" / member).is_file()
            else None),
        "child_race_parked_source_sha256": (
            sha256(out / "child-parked.txt")
            if args.child_race and (out / "child-parked.txt").is_file()
            else None),
        "directory_race_counterfeit_sha256": (
            sha256(source) if (args.directory_race or args.child_race) and
            source.is_file() else None),
        "fat_attribute_source_set": attr_set_results,
        "attributes_probe": str(attrs_probe) if attrs_probe else None,
        "attributes_probe_sha256": sha256(attrs_probe) if attrs_probe else None,
        "fat_attribute_roundtrip": attribute_roundtrip if args.fat_attributes else None,
        "gui_extraction_command": extraction_command,
        "gui_extraction_exit_status": extraction_exit_status,
        "profile1_method_observed": ("BoltLite" if
                                     args.profile1 and "BoltLite" in method_line and
                                     "boltlz" not in method_line.lower()
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
        "source_sha256": sha256(source_for_hash),
        "linux_test_status": linux_test_status,
        "mini_test_status": mini_test_status,
        "linux_extract_status": extract_status,
        "extracted_sha256": sha256(output) if output.is_file() else None,
        "payload_byte_identical": (output.is_file() and
                                   output.read_bytes() == payload
                                   if archive.is_file() else None),
        "wine_exit_status": process.returncode,
        "artifacts": {path.name: sha256(path) for path in out.iterdir()
                      if path.is_file()},
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print(json.dumps(record, indent=2))
    if result not in ("passed_under_wine", "passed_under_wine_refused_substitution") or process.returncode != 0:
        raise SystemExit("Win64 FLTK Wine create failed; evidence: " + str(out))


if __name__ == "__main__":
    main()
