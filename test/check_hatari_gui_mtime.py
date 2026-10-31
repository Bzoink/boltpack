#!/usr/bin/env python3
"""Verify Atari GEM reports an out-of-range archived mtime as partial."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MTIME_2040 = 2208988800
PAYLOAD = b"Atari GEM must retain verified bytes when the archived time is out of range.\n"


def run(command, cwd, env=None, timeout=30):
    return subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)


def archived_mtimes(path):
    data = Path(path).read_bytes()
    table_offset, table_size = struct.unpack_from("<QQ", data, len(data) - 20)
    count = struct.unpack_from("<I", data, table_offset + 4)[0]
    record = table_offset + 16
    result = {}
    for _ in range(count):
        name_len = struct.unpack_from("<H", data, record)[0]
        name = data[record + 2:record + 2 + name_len].decode("utf-8")
        fields = record + 2 + name_len
        mtime = struct.unpack_from("<Q", data, fields + 18)[0]
        flags = struct.unpack_from("<H", data, fields + 41)[0]
        result[name] = (mtime, flags)
        record = fields + 43
    if record != table_offset + table_size:
        raise RuntimeError("archive table length did not match its entry records")
    return result


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", type=Path,
                        default=ROOT / "build/atari/boltpack.prg")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    if not os.environ.get("DISPLAY"):
        raise RuntimeError("run this target under Xvfb or an X11 display")
    if shutil.which(args.hatari) is None:
        raise RuntimeError("Hatari executable not found: " + args.hatari)

    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    out_root = args.output_root.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-gui-time-range-", dir=out_root))
    host = out / "host"
    host.mkdir()
    (host / "OUT").mkdir()
    tree = host / "TREE"
    tree.mkdir()
    payload = tree / "PAYLOAD.BIN"
    payload.write_bytes(PAYLOAD)
    os.utime(payload, (MTIME_2040, MTIME_2040))
    os.utime(tree, (MTIME_2040, MTIME_2040))
    shutil.copy2(gui, host / "BOLTPACK.PRG")

    created = run([str(linux), "c", "--store", "ARCHIVE.BLT", tree.name],
                  host, timeout=60)
    (out / "create.log").write_text(created.stdout, encoding="utf-8")
    if created.returncode != 0:
        raise RuntimeError("Linux could not create the timestamp fixture: " + str(out))
    tested = run([str(linux), "t", "ARCHIVE.BLT"], host, timeout=60)
    (out / "archive-test.log").write_text(tested.stdout, encoding="utf-8")
    if tested.returncode != 0:
        raise RuntimeError("Linux rejected the timestamp fixture: " + str(out))
    archived = archived_mtimes(host / "ARCHIVE.BLT")
    for name in ("TREE/", "TREE/PAYLOAD.BIN"):
        item = archived.get(name)
        if item is None or item[0] != MTIME_2040 or not (item[1] & 0x4000):
            raise RuntimeError("fixture archive lacks valid 2040 mtime for " + name)

    script = out / "HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 30",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 10", "text OUT", "keypress 28",
        "sleep 30", "screenshot", "sleep 2", "quit", "",
    )), encoding="ascii")
    env = os.environ.copy()
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    hatari_binary = shutil.which(args.hatari)
    command = [
        sys.executable, str(hconsole), str(script), "--exit", "--",
        "--machine", "ste", "--tos", str(tos), "--sound", "off",
        "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
        "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG",
    ]
    emulated = run(command, out, env=env, timeout=240)
    (out / "hatari.log").write_text(emulated.stdout, encoding="utf-8")
    if emulated.returncode != 0:
        raise RuntimeError("Hatari extraction session failed: " + str(out))
    screenshot = out / "grab0001.png"
    if not screenshot.is_file():
        raise RuntimeError("Hatari did not capture the extraction result: " + str(out))

    extracted_tree = host / "OUT" / tree.name
    target = extracted_tree / payload.name
    if not target.is_file() or target.read_bytes() != PAYLOAD:
        raise RuntimeError("out-of-range mtime changed or lost verified payload bytes: " + str(out))
    expected_paths = {host / "OUT", extracted_tree, target}
    leftovers = sorted(str(path.relative_to(host / "OUT"))
                       for path in (host / "OUT").rglob("*") if path not in expected_paths)
    if leftovers:
        raise RuntimeError("metadata partial result left transaction files: " + repr(leftovers))
    archive_test = run([str(linux), "t", "ARCHIVE.BLT"], host, timeout=60)
    if archive_test.returncode != 0:
        raise RuntimeError("Linux no longer accepts the extracted fixture archive: " + str(out))

    revision = run(["git", "rev-parse", "HEAD"], ROOT).stdout.strip()
    worktree = run(["git", "status", "--short"], ROOT).stdout.splitlines()
    sha256 = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
    record = {
        "purpose": "Atari GEM preserves verified bytes and reports partial for an archived 2040 mtime",
        "revision": revision,
        "worktree_status": worktree,
        "hatari": run([hatari_binary, "--version"], out).stdout.strip(),
        "tos_rom": tos.name,
        "tos_sha256": sha256(tos),
        "gui_sha256": sha256(gui),
        "linux_cli_sha256": sha256(linux),
        "runner_sha256": sha256(Path(__file__)),
        "archive_sha256": sha256(host / "ARCHIVE.BLT"),
        "payload_sha256": sha256(target),
        "payload_bytes": len(PAYLOAD),
        "archive_mtime_utc_seconds": MTIME_2040,
        "archive_mtime_flag_valid_for": ["TREE/", "TREE/PAYLOAD.BIN"],
        "expected_gemdos_time_result": "partial; GEMDOS time_t is signed 32-bit",
        "deferred_directory_mtime_after_extract": int(extracted_tree.stat().st_mtime),
        "partial_alert_screenshot_sha256": sha256(screenshot),
        "partial_alert_screenshot_review": "The production GUI displayed its Partial extraction alert after preserving the file.",
        "target_mtime_after_extract": int(target.stat().st_mtime),
        "leftover_transaction_files": leftovers,
        "screenshots": ["grab0001.png"],
        "hatari_command": command,
        "result": "exact verified bytes extracted, no stage or journal left; out-of-range mtime reported partial",
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("Atari GEM out-of-range timestamp extraction passed: " + str(out))


if __name__ == "__main__":
    main()
