#!/usr/bin/env python3
"""Exercise Linux FLTK GUI archive open and nested extraction under Xvfb."""

import argparse
import hashlib
import json
import mmap
import os
import pathlib
import shlex
import stat
import struct
import subprocess
import tempfile


SCRIPT = r'''set -eu
"$1" &
gui_pid=$!
trap 'kill "$gui_pid" 2>/dev/null || true' EXIT
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
choose_path() {
    title=$1
    path=$2
    dialog=$(xdotool search --sync --onlyvisible --name "^$title$" | head -1)
    xdotool windowfocus --sync "$dialog"
    xdotool key ctrl+l
    sleep 0.3
    xdotool type --clearmodifiers "$path"
    xdotool key Return
    sleep 0.5
    eval "$(xdotool getwindowgeometry --shell "$dialog")"
    xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
}
choose_working_directory() {
    title=$1
    dialog=$(xdotool search --sync --onlyvisible --name "^$title$" | head -1)
    xdotool windowfocus --sync "$dialog"
    # In this controlled Xvfb profile GTK exposes the process working folder
    # as a sidebar shortcut; selecting it avoids ambiguous location-entry
    # behavior for directory-only choosers.
    xdotool mousemove --window "$dialog" 60 135 click 1
    sleep 0.3
    eval "$(xdotool getwindowgeometry --shell "$dialog")"
    xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
}
# Exercise the production save flow with a single selected source file.
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+n
create_dialog=$(xdotool search --sync --onlyvisible --name '^Create Archive$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$create_dialog")"
# This build includes the network action between Save and Cancel.
xdotool mousemove --window "$create_dialog" 105 "$((HEIGHT - 24))" click 1
choice=$(xdotool search --sync --onlyvisible --name '^$' | tail -1)
xdotool windowfocus --sync "$choice"
xdotool key Tab Tab Return
choose_path 'Select files to archive' "$2"
choose_path 'Save archive as' "$3/gui-created"
for attempt in $(seq 1 100); do
    test -s "$3/gui-created.blt" && break
    sleep 0.1
done
test -s "$3/gui-created.blt"
# Open and extract an independently CLI-created nested archive as well.
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+o
choose_path 'Open BoltPack Archive' "$4"
sleep 1
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+e
choose_path 'Select destination folder' "$5"
for attempt in $(seq 1 100); do
    test -s "$6" && break
    sleep 0.1
done
test -s "$6"
# Select both nested paths with the same leaf and flatten into the process
# working directory. The chooser's sidebar target is that directory.
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+o
choose_path 'Open BoltPack Archive' "$7"
sleep 0.5
xdotool mousemove 50 78 click 1
xdotool keydown ctrl
xdotool mousemove 50 98 click 1
xdotool keyup ctrl
xdotool mousemove 35 31 click 1
sleep 0.2
xdotool mousemove 75 97 click 1
choice=$(xdotool search --sync --onlyvisible --name '^$' | tail -1)
eval "$(xdotool getwindowgeometry --shell "$choice")"
xdotool mousemove --window "$choice" "$((WIDTH / 2 - 31))" \
    "$((HEIGHT - 24))" click 1
choose_working_directory 'Select destination folder'
for attempt in $(seq 1 100); do
    test -s "$8/same-01.txt" && test -s "$8/same-02.txt" && break
    sleep 0.1
done
test -s "$8/same-01.txt"
test -s "$8/same-02.txt"
kill -TERM "$gui_pid"
trap - EXIT
wait "$gui_pid" >/dev/null 2>&1 || true
'''


ZSTD_CREATE_SCRIPT = r'''set -eu
"$1" &
gui_pid=$!
trap 'kill "$gui_pid" 2>/dev/null || true' EXIT
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
choose_path() {
    title=$1
    path=$2
    dialog=$(xdotool search --sync --onlyvisible --name "^$title$" | head -1)
    xdotool windowfocus --sync "$dialog"
    xdotool key ctrl+l
    sleep 0.3
    xdotool type --clearmodifiers "$path"
    xdotool key Return
    sleep 0.5
    eval "$(xdotool getwindowgeometry --shell "$dialog")"
    xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
}
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+n
create_dialog=$(xdotool search --sync --onlyvisible --name '^Create Archive$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$create_dialog")"
xdotool mousemove --window "$create_dialog" 105 "$((HEIGHT - 24))" click 1
choice=$(xdotool search --sync --onlyvisible --name '^$' | tail -1)
xdotool windowfocus --sync "$choice"
xdotool key Tab Tab Return
files_dialog=$(xdotool search --sync --onlyvisible --name '^Select files to archive$' | head -1)
xdotool windowfocus --sync "$files_dialog"
xdotool key ctrl+l
sleep 0.3
xdotool type --clearmodifiers "$2"
xdotool key Return
sleep 0.5
for attempt in $(seq 1 8); do
    if ! xdotool search --onlyvisible --name '^Select files to archive$' \
         >/dev/null 2>&1; then
        break
    fi
    xdotool mousemove --window "$files_dialog" 250 180 click 1
    xdotool key ctrl+a
    sleep 0.3
    eval "$(xdotool getwindowgeometry --shell "$files_dialog")"
    xdotool mousemove --window "$files_dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
    sleep 0.4
done
if xdotool search --onlyvisible --name '^Select files to archive$' \
   >/dev/null 2>&1; then
    echo "multi-file source chooser did not close" >&2
    exit 1
fi
choose_path 'Save archive as' "$3/zstd-created"
for attempt in $(seq 1 100); do
    test -s "$3/zstd-created.blt" && break
    sleep 0.1
done
test -s "$3/zstd-created.blt"
kill -TERM "$gui_pid"
trap - EXIT
wait "$gui_pid" >/dev/null 2>&1 || true
'''

EXTRACTION_FAILURE_SCRIPT = r'''set -eu
case "$4" in
    metadata) failure_env=BOLTPACK_TEST_FAIL_FUTIMENS ;;
    write) failure_env=BOLTPACK_TEST_FAIL_FWRITE ;;
    flush) failure_env=BOLTPACK_TEST_FAIL_FFLUSH ;;
    close) failure_env=BOLTPACK_TEST_FAIL_FCLOSE ;;
    cancel) failure_env=BOLTPACK_TEST_DELAY_FIRST_WRITE ;;
    *) failure_env= ;;
esac
if test -n "$failure_env"; then
    env "$failure_env=1" LD_PRELOAD="$5" "$1" &
else
    "$1" &
fi
gui_pid=$!
trap 'kill "$gui_pid" 2>/dev/null || true' EXIT
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
choose_path() {
    title=$1
    path=$2
    dialog=$(xdotool search --sync --onlyvisible --name "^$title$" | head -1)
    xdotool windowfocus --sync "$dialog"
    xdotool key ctrl+l
    sleep 0.3
    xdotool type --clearmodifiers "$path"
    xdotool key Return
    sleep 0.5
    eval "$(xdotool getwindowgeometry --shell "$dialog")"
    xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
}
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+o
choose_path 'Open BoltPack Archive' "$2"
sleep 0.3
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+e
choose_path 'Select destination folder' "$3"
progress=
if test "$4" = cancel; then
    for attempt in $(seq 1 400); do
        progress=$(xdotool search --onlyvisible --name '^Extracting' 2>/dev/null | head -1 || true)
        test -n "$progress" && break
        sleep 0.02
    done
    if test -z "$progress"; then
        echo "extraction progress window did not appear" >&2
        exit 1
    fi
    # Xvfb has no window manager to acknowledge focus requests for this
    # transient FLTK window. Send the click directly to cancel.
    xdotool mousemove --window "$progress" 200 137 click 1
else
    # Metadata failure can finish before Xvfb observes the progress window.
    # Wait for the worker to reach its error alert, then dismiss the alert.
    sleep 1
    xdotool key Return
fi
for attempt in $(seq 1 200); do
    if ! xdotool search --onlyvisible --name '^Extracting' >/dev/null 2>&1; then
        break
    fi
    sleep 0.05
done
if test "$4" = metadata; then
    # A metadata restoration error is reported in a modal alert after the
    # progress window closes. Dismiss it before checking publication state.
    sleep 0.2
fi
kill -TERM "$gui_pid"
trap - EXIT
wait "$gui_pid" >/dev/null 2>&1 || true
'''


PREFIX_FAILURE_SCRIPT = r'''set -eu
"$1" &
gui_pid=$!
trap 'kill "$gui_pid" 2>/dev/null || true' EXIT
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
choose_path() {
    title=$1
    path=$2
    dialog=$(xdotool search --sync --onlyvisible --name "^$title$" | head -1)
    xdotool windowfocus --sync "$dialog"
    xdotool key ctrl+l
    sleep 0.3
    xdotool type --clearmodifiers "$path"
    xdotool key Return
    sleep 0.5
    eval "$(xdotool getwindowgeometry --shell "$dialog")"
    xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
}
choose_working_directory() {
    title=$1
    dialog=$(xdotool search --sync --onlyvisible --name "^$title$" | head -1)
    xdotool windowfocus --sync "$dialog"
    xdotool mousemove --window "$dialog" 60 135 click 1
    sleep 0.3
    eval "$(xdotool getwindowgeometry --shell "$dialog")"
    xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
        "$((HEIGHT - 28))" click 1
}
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+o
choose_path 'Open BoltPack Archive' "$2"
sleep 0.4
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+e
choose_working_directory 'Select destination folder'
sleep 1
xdotool key Return
kill -TERM "$gui_pid"
trap - EXIT
wait "$gui_pid" >/dev/null 2>&1 || true
'''


UNSAFE_NAME_SCRIPT = r'''set -eu
"$1" &
gui_pid=$!
trap 'kill "$gui_pid" 2>/dev/null || true' EXIT
main=$(xdotool search --sync --onlyvisible --name '^BoltPack$' | head -1)
xdotool windowfocus --sync "$main"
xdotool key --window "$main" ctrl+o
dialog=$(xdotool search --sync --onlyvisible --name '^Open BoltPack Archive$' | head -1)
xdotool windowfocus --sync "$dialog"
xdotool key ctrl+l
sleep 0.3
xdotool type --clearmodifiers "$2"
xdotool key Return
sleep 0.5
eval "$(xdotool getwindowgeometry --shell "$dialog")"
xdotool mousemove --window "$dialog" "$((WIDTH - 55))" \
    "$((HEIGHT - 28))" click 1
# Invalid archive names fail while opening. Dismiss the error alert if shown,
# then verify the GUI did not progress to extraction or modify the filesystem.
sleep 1
xdotool key Return
sleep 0.2
xdotool key --window "$main" ctrl+e
sleep 0.5
if xdotool search --onlyvisible --name '^Select destination folder$' >/dev/null 2>&1; then
    echo "GUI accepted unsafe-name archive and enabled extraction" >&2
    exit 1
fi
kill -TERM "$gui_pid"
trap - EXIT
wait "$gui_pid" >/dev/null 2>&1 || true
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gui", required=True, type=pathlib.Path)
    parser.add_argument("--cli", required=True, type=pathlib.Path)
    parser.add_argument("--mini", required=True, type=pathlib.Path)
    parser.add_argument("--mini-runner", default="",
                        help="optional command prefix, such as qemu-i386")
    parser.add_argument("--prefix-fixture", required=True, type=pathlib.Path)
    args = parser.parse_args()
    gui = args.gui.resolve()
    cli = args.cli.resolve()
    mini = args.mini.resolve()
    mini_runner = shlex.split(args.mini_runner)
    prefix_fixture = args.prefix_fixture.resolve()
    manifest_path = (pathlib.Path(__file__).resolve().parent /
                     "reference/interoperability/linux-fltk-gui.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for executable in ("xvfb-run", "xdotool"):
        if subprocess.run(["sh", "-c", f"command -v {executable}"],
                          capture_output=True).returncode != 0:
            raise SystemExit(f"required GUI test tool is missing: {executable}")

    with tempfile.TemporaryDirectory(prefix="boltpack-linux-gui-") as temp:
        root = pathlib.Path(temp)
        source_root = root / "source"
        profile_source_root = root / "profile-source"
        output = root / "output"
        create_output = root / "created"
        flatten_source = root / "flatten-source"
        flatten_output = root / "flatten-output"
        cancel_source = root / "cancel-source"
        config_home = root / "xdg-config"
        source_root.mkdir()
        profile_source_root.mkdir()
        output.mkdir()
        create_output.mkdir()
        flatten_source.mkdir()
        flatten_output.mkdir()
        cancel_source.mkdir()
        config_home.mkdir()
        prefs = config_home / "boltpack"
        prefs.mkdir()
        (prefs / "gui.conf").write_text(
            "method=1\nlevel=1\ninterchange_profile=1\n", encoding="ascii")
        entry = manifest["entries"][0]
        relative_name = pathlib.PurePosixPath(entry["archive_name"])
        source = source_root.joinpath(*relative_name.parts)
        source.parent.mkdir(parents=True, exist_ok=True)
        payload = entry["content"].encode("utf-8")
        if len(payload) != entry["size"] or hashlib.sha256(payload).hexdigest() != entry["sha256"]:
            raise SystemExit("GUI fixture does not match its reference manifest")
        source.write_bytes(payload)
        source.chmod(int(entry["mode"], 8))
        os.utime(source, (entry["mtime_epoch"], entry["mtime_epoch"]))
        directory_entry = manifest["directories"][0]
        source.parent.chmod(int(directory_entry["mode"], 8))
        os.utime(source.parent, (directory_entry["mtime_epoch"],
                                 directory_entry["mtime_epoch"]))
        created_profile = manifest["created_profile"]
        if created_profile["archive_name"] != manifest["created_entry_name"]:
            raise SystemExit("GUI Profile 1 manifest entry names disagree")
        profile_source = profile_source_root / created_profile["archive_name"]
        profile_payload = b"A" * created_profile["size"]
        if hashlib.sha256(profile_payload).hexdigest() != created_profile["sha256"]:
            raise SystemExit("GUI Profile 1 fixture does not match its reference manifest")
        profile_source.write_bytes(profile_payload)
        os.utime(profile_source, (-1, -1))
        zstd_source = root / "zstd-source"
        zstd_source.mkdir()
        zstd_payloads = {
            "first.bin": bytes(range(256)) * 129,
            "second.bin": bytes(reversed(range(256))) * 131,
        }
        for name, data in zstd_payloads.items():
            (zstd_source / name).write_bytes(data)
            os.utime(zstd_source / name, (-1, -1))
        os.utime(zstd_source, (-1, -1))
        zstd_config_home = root / "zstd-config"
        zstd_prefs = zstd_config_home / "boltpack"
        zstd_prefs.mkdir(parents=True)
        (zstd_prefs / "gui.conf").write_text(
            "method=4\nlevel=1\nzstd_mt=1\ninterchange_profile=0\n",
            encoding="ascii")
        flatten_entries = manifest["flattened_collision"]["entries"]
        existing_output = manifest["flattened_collision"]["existing_output"]
        (flatten_output / existing_output["name"]).write_text(
            existing_output["content"], encoding="utf-8")
        for flatten_entry in flatten_entries:
            relative = pathlib.PurePosixPath(flatten_entry["archive_name"])
            path = flatten_source.joinpath(*relative.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(flatten_entry["content"].encode("utf-8"))
        cancel_entry = manifest["cancelled_extraction"]
        cancel_input = cancel_source / cancel_entry["archive_name"]
        with cancel_input.open("wb") as cancel_fp:
            chunk = bytes(range(256)) * 4096
            for _ in range(cancel_entry["size"] // len(chunk)):
                cancel_fp.write(chunk)
        archive = root / "fixture.bolt"
        flatten_archive = root / "flatten-collision.bolt"
        cancel_archive = root / "cancel-extract.bolt"
        prefix_archive = root / "prefix-conflict.bolt"
        unsafe_archive = root / "unsafe-name.bolt"
        created_archive = create_output / manifest["created_archive_name"]
        zstd_archive = create_output / "zstd-created.blt"
        subprocess.run([str(cli), "c", str(archive),
                        relative_name.parent.as_posix()],
                       cwd=root / "source", check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([str(cli), "c", str(flatten_archive),
                        *(item["archive_name"] for item in flatten_entries)],
                       cwd=flatten_source, check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([str(cli), "c", "-0", str(cancel_archive),
                        cancel_entry["archive_name"]], cwd=cancel_source,
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([str(prefix_fixture), str(prefix_archive)], check=True)
        subprocess.run([str(prefix_fixture), str(unsafe_archive), "unsafe"],
                       check=True)
        unsafe_listing = subprocess.run([str(cli), "l", str(unsafe_archive)],
                                        capture_output=True)
        if unsafe_listing.returncode == 0:
            raise SystemExit("unsafe-name fixture is accepted by the CLI reader")
        prefix_listing = subprocess.run([str(cli), "l", str(prefix_archive)],
                                        check=True, capture_output=True,
                                        text=True)
        if "node" not in prefix_listing.stdout or "node/child" not in prefix_listing.stdout:
            raise SystemExit("prefix-conflict fixture does not contain both paths")
        sentinel = flatten_output / cancel_entry["archive_name"]
        sentinel.write_text(cancel_entry["existing_content"], encoding="utf-8")
        command = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
                   "sh", "-c", SCRIPT, "linux-gui-test", str(gui),
                   str(profile_source), str(create_output), str(archive), str(output),
                   str(output.joinpath(*relative_name.parts)),
                   str(flatten_archive), str(flatten_output)]
        environment = os.environ.copy()
        environment["XDG_CONFIG_HOME"] = str(config_home)

        metadata_shim = root / "fail-futimens.so"
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC",
                        str(pathlib.Path(__file__).with_name(
                            "linux_stage_failure_interposer.c")), "-ldl", "-o",
                        str(metadata_shim)], check=True)
        subprocess.run(command, check=True, timeout=120, env=environment,
                       cwd=flatten_output)
        zstd_environment = environment.copy()
        zstd_environment["XDG_CONFIG_HOME"] = str(zstd_config_home)
        zstd_create_command = [
            "xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
            "sh", "-c", ZSTD_CREATE_SCRIPT, "linux-gui-zstd-create",
            str(gui), str(zstd_source), str(create_output)]
        subprocess.run(zstd_create_command, check=True, timeout=120,
                       env=zstd_environment, cwd=flatten_output)
        if not zstd_archive.is_file():
            raise SystemExit("GUI parallel Zstd creation did not publish an archive")
        subprocess.run([str(cli), "t", str(zstd_archive)], check=True,
                       stdout=subprocess.DEVNULL)
        zstd_bytes = zstd_archive.read_bytes()
        zstd_table_offset = struct.unpack_from(
            "<Q", zstd_bytes, len(zstd_bytes) - 20)[0]
        zstd_entry_count = struct.unpack_from(
            "<I", zstd_bytes, zstd_table_offset + 4)[0]
        zstd_entries = {}
        zstd_record = zstd_table_offset + 16
        for _ in range(zstd_entry_count):
            zstd_name_length = struct.unpack_from("<H", zstd_bytes,
                                                  zstd_record)[0]
            zstd_name = zstd_bytes[zstd_record + 2:
                                   zstd_record + 2 + zstd_name_length].decode("utf-8")
            zstd_fields = zstd_record + 2 + zstd_name_length
            zstd_mtime = struct.unpack_from("<Q", zstd_bytes,
                                            zstd_fields + 18)[0]
            zstd_flags = struct.unpack_from("<H", zstd_bytes,
                                            zstd_fields + 41)[0]
            zstd_entries[zstd_name] = (zstd_mtime, zstd_flags)
            zstd_record = zstd_fields + 43
        if set(zstd_entries) != {"first.bin", "second.bin"}:
            raise SystemExit(
                "GUI parallel Zstd archive has unexpected entries: " +
                repr(sorted(zstd_entries)))
        if any(mtime != 0 or flags & 0x4000
               for mtime, flags in zstd_entries.values()):
            raise SystemExit("GUI parallel Zstd creation marked pre-epoch times known")
        zstd_output = root / "zstd-extract"
        subprocess.run([str(cli), "x", "-o", str(zstd_output),
                        str(zstd_archive)], check=True,
                       stdout=subprocess.DEVNULL)
        for name, expected_data in zstd_payloads.items():
            if (zstd_output / name).read_bytes() != expected_data:
                raise SystemExit("GUI parallel Zstd payload differs after extraction")
        extraction_failure_command = [
            "xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
            "sh", "-c", EXTRACTION_FAILURE_SCRIPT,
            "linux-gui-extraction-failure", str(gui), str(cancel_archive),
            str(flatten_output), "cancel", str(metadata_shim)]
        subprocess.run(extraction_failure_command, check=True, timeout=60,
                       env=environment, cwd=flatten_output)
        extracted = output.joinpath(*relative_name.parts)
        restored = extracted.read_bytes()
        if (len(restored) != entry["size"] or
                hashlib.sha256(restored).hexdigest() != entry["sha256"]):
            raise SystemExit("Linux FLTK GUI extraction differs from source")
        restored_stat = extracted.stat()
        if stat.S_IMODE(restored_stat.st_mode) != int(entry["mode"], 8):
            raise SystemExit("Linux FLTK GUI extraction did not restore file mode")
        if int(restored_stat.st_mtime) != entry["mtime_epoch"]:
            raise SystemExit("Linux FLTK GUI extraction did not restore file mtime")
        restored_directory = extracted.parent.stat()
        if (stat.S_IMODE(restored_directory.st_mode) !=
                int(directory_entry["mode"], 8)):
            raise SystemExit("Linux FLTK GUI extraction did not restore directory mode")
        if int(restored_directory.st_mtime) != directory_entry["mtime_epoch"]:
            raise SystemExit("Linux FLTK GUI extraction did not restore directory mtime")
        subprocess.run([str(cli), "t", str(created_archive)], check=True,
                       stdout=subprocess.DEVNULL)
        created_bytes = created_archive.read_bytes()
        table_offset = struct.unpack_from("<Q", created_bytes,
                                          len(created_bytes) - 20)[0]
        entry_count = struct.unpack_from("<I", created_bytes,
                                         table_offset + 4)[0]
        if entry_count != 1:
            raise SystemExit("GUI Profile 1 fixture has an unexpected entry count")
        record = table_offset + 16
        name_length = struct.unpack_from("<H", created_bytes, record)[0]
        fields = record + 2 + name_length
        stored_mtime = struct.unpack_from("<Q", created_bytes, fields + 18)[0]
        entry_flags = struct.unpack_from("<H", created_bytes, fields + 41)[0]
        if stored_mtime != 0 or entry_flags & 0x4000:
            raise SystemExit("GUI stored a pre-1970 source time as known")
        entry_offset = struct.unpack_from("<Q", created_bytes, fields + 26)[0]
        inline = entry_offset + 4 + name_length
        method = created_bytes[inline + 20]
        block_size = struct.unpack_from("<I", created_bytes, inline + 22)[0]
        if (method, block_size) != (1, 16384):
            raise SystemExit(
                "GUI Profile 1 did not create BoltLite/16 KiB: "
                f"method={method} block_size={block_size}")
        mini_test = subprocess.run(
            [*mini_runner, str(mini), "t", str(created_archive)],
            capture_output=True, text=True)
        if mini_test.returncode != 0:
            raise SystemExit(
                "Linux FLTK GUI archive is not accepted by the mini reader: " +
                mini_test.stderr)
        created_output = root / "created-extract"
        subprocess.run([str(cli), "x", "-o", str(created_output),
                        str(created_archive)], check=True,
                       stdout=subprocess.DEVNULL)
        created_entry = pathlib.PurePosixPath(created_profile["archive_name"])
        listed = subprocess.run([str(cli), "l", str(created_archive)],
                                check=True, capture_output=True, text=True)
        if created_entry.as_posix() not in listed.stdout:
            raise SystemExit("Linux FLTK GUI archive has an unexpected entry name")
        created_payload = created_output.joinpath(*created_entry.parts).read_bytes()
        if (len(created_payload) != created_profile["size"] or
                hashlib.sha256(created_payload).hexdigest() != created_profile["sha256"]):
            raise SystemExit("Linux FLTK GUI creation differs from source")
        mini_output = root / "created-mini-extract"
        mini_extract = subprocess.run(
            [*mini_runner, str(mini), "x", str(created_archive),
             str(mini_output)], capture_output=True, text=True)
        if mini_extract.returncode != 0:
            raise SystemExit(
                "Linux mini reader could not extract GUI archive: " +
                mini_extract.stderr)
        mini_created_payload = mini_output.joinpath(*created_entry.parts).read_bytes()
        if (len(mini_created_payload) != created_profile["size"] or
                hashlib.sha256(mini_created_payload).hexdigest() != created_profile["sha256"]):
            raise SystemExit("Linux mini extraction differs from GUI source")
        for flatten_entry in flatten_entries:
            flattened = (flatten_output /
                         flatten_entry["flattened_name"]).read_bytes()
            if (len(flattened) != flatten_entry["size"] or
                    hashlib.sha256(flattened).hexdigest() !=
                    flatten_entry["sha256"]):
                raise SystemExit(
                    f"flattened GUI output differs: {flatten_entry['flattened_name']}")
        if ((flatten_output / existing_output["name"]).read_text(
                encoding="utf-8") != existing_output["content"]):
            raise SystemExit("flattened GUI extraction replaced an existing file")
        if sentinel.read_text(encoding="utf-8") != cancel_entry["existing_content"]:
            raise SystemExit("cancelled GUI extraction replaced the existing file")
        leftovers = [path.name for path in flatten_output.iterdir()
                     if path.name.startswith(".boltpack-gui-")]
        if leftovers:
            raise SystemExit(f"cancelled GUI extraction left stages: {leftovers}")
        # Reuse the stored fixture after the cancellation case; only its final
        # payload byte changes for the checksum-failure pass.
        corrupt_archive = cancel_archive
        payload_marker = bytes(range(256)) * 4
        with corrupt_archive.open("r+b") as corrupt_fp:
            with mmap.mmap(corrupt_fp.fileno(), 0) as corrupt_map:
                payload_start = corrupt_map.find(payload_marker)
                if payload_start < 0:
                    raise SystemExit("could not locate stored payload in GUI fixture")
                payload_end = payload_start + cancel_entry["size"]
                if payload_end > len(corrupt_map):
                    raise SystemExit("stored payload extends beyond GUI fixture")
                corrupt_map[payload_end - 1] ^= 0x01
                corrupt_map.flush()
        integrity = subprocess.run([str(cli), "t", str(corrupt_archive)],
                                   capture_output=True)
        if (integrity.returncode == 0 or
                b"checksum" not in integrity.stdout.lower() + integrity.stderr.lower()):
            raise SystemExit("corrupt GUI fixture did not fail at a checksum")
        error_output = root / "error-output"
        error_output.mkdir()
        error_sentinel = error_output / cancel_entry["archive_name"]
        error_sentinel.write_text(cancel_entry["existing_content"],
                                  encoding="utf-8")
        error_command = [
            "xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
            "sh", "-c", EXTRACTION_FAILURE_SCRIPT,
            "linux-gui-corrupt-test", str(gui), str(corrupt_archive),
            str(error_output), "finish"]
        subprocess.run(error_command, check=True, timeout=60,
                       env=environment, cwd=error_output)
        if error_sentinel.read_text(encoding="utf-8") != cancel_entry["existing_content"]:
            raise SystemExit("corrupt GUI extraction replaced the existing file")
        error_leftovers = [path.name for path in error_output.iterdir()
                           if path.name.startswith(".boltpack-gui-")]
        if error_leftovers:
            raise SystemExit(f"corrupt GUI extraction left stages: {error_leftovers}")
        for failure_mode in ("write", "flush", "close", "metadata"):
            failure_output = root / f"{failure_mode}-error-output"
            failure_parent = failure_output / "nested"
            failure_parent.mkdir(parents=True)
            failure_sentinel = failure_parent / "payload.txt"
            failure_content = f"preserve this file after {failure_mode} failure\n"
            failure_sentinel.write_text(failure_content, encoding="utf-8")
            failure_command = [
                "xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
                "sh", "-c", EXTRACTION_FAILURE_SCRIPT,
                f"linux-gui-{failure_mode}-failure", str(gui), str(archive),
                str(failure_output), failure_mode, str(metadata_shim)]
            subprocess.run(failure_command, check=True, timeout=60,
                           env=environment, cwd=failure_output)
            if failure_sentinel.read_text(encoding="utf-8") != failure_content:
                raise SystemExit(
                    f"{failure_mode} failure replaced the existing destination")
            failure_leftovers = [
                path.name for path in failure_parent.iterdir()
                if path.name.startswith(".boltpack-gui-")]
            if failure_leftovers:
                raise SystemExit(
                    f"{failure_mode} failure left stages: {failure_leftovers!r}")
        prefix_output = root / "prefix-output"
        prefix_output.mkdir()
        prefix_command = [
            "xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
            "sh", "-c", PREFIX_FAILURE_SCRIPT,
            "linux-gui-prefix-conflict", str(gui), str(prefix_archive),
            str(prefix_output)]
        prefix_result = subprocess.run(prefix_command, timeout=60,
                                       env=environment, cwd=prefix_output,
                                       capture_output=True, text=True)
        if prefix_result.returncode != 0:
            raise SystemExit("GUI path-conflict fixture failed: " +
                             prefix_result.stdout + prefix_result.stderr)
        if list(prefix_output.iterdir()):
            raise SystemExit(
                "GUI path-conflict preflight left output behind: " +
                repr([path.name for path in prefix_output.iterdir()]))
        unsafe_output = root / "unsafe-output"
        unsafe_output.mkdir()
        unsafe_command = [
            "xvfb-run", "-a", "-s", "-screen 0 1280x1024x24",
            "sh", "-c", UNSAFE_NAME_SCRIPT,
            "linux-gui-unsafe-name", str(gui), str(unsafe_archive),
            str(unsafe_output)]
        unsafe_result = subprocess.run(unsafe_command, timeout=60,
                                       env=environment, cwd=unsafe_output,
                                       capture_output=True, text=True)
        if unsafe_result.returncode != 0:
            raise SystemExit("GUI unsafe-name fixture failed: " +
                             unsafe_result.stdout + unsafe_result.stderr)
        if list(unsafe_output.iterdir()) or (root / "x").exists():
            raise SystemExit("GUI unsafe-name preflight changed the destination "
                             "or wrote outside it")
        print("Linux FLTK GUI Profile 1 creation, mini-reader extraction, unsafe-name and path-conflict preflight, open/extract/metadata/flatten/cancel/write/flush/close/corruption interoperability passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
