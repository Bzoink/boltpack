#!/usr/bin/env python3
"""Verify Atari recovery refuses a checksummed journal that escapes OUT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(command, cwd, timeout=30):
    return subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)


def path_field(value):
    raw = value.encode("ascii")
    if len(raw) > 125:
        raise ValueError("recovery path exceeds GEMDOS limit")
    return raw + bytes(126 - len(raw))


def make_journal(target, temporary, backup, old, new):
    import struct
    import zlib

    record = bytearray(414)
    record[:8] = b"BPTXN6\0\0"
    struct.pack_into(">I", record, 8, 1)
    struct.pack_into(">I", record, 16, 1)
    struct.pack_into(">I", record, 20, len(old))
    struct.pack_into(">I", record, 24, zlib.crc32(old))
    struct.pack_into(">I", record, 28, len(new))
    struct.pack_into(">I", record, 32, zlib.crc32(new))
    record[36:162] = path_field(target)
    record[162:288] = path_field(temporary)
    record[288:414] = path_field(backup)
    struct.pack_into(">I", record, 12, zlib.crc32(record))
    return bytes(record)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tos", required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", default="/usr/share/hatari/hconsole/hconsole.py")
    parser.add_argument("--gui-prg", required=True)
    parser.add_argument("--linux-cli", required=True)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()

    tos = Path(args.tos).resolve()
    hatari = shutil.which(args.hatari) or args.hatari
    hconsole = str(Path(args.hconsole).resolve())
    gui = Path(args.gui_prg).resolve()
    linux = Path(args.linux_cli).resolve()
    if args.output:
        out = Path(args.output).resolve()
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix="atari-journal-path-", dir=ROOT / "build"))
    host = out / "host"
    output_dir = host / "OUT"
    evil_dir = host / "EVIL"
    output_dir.mkdir(parents=True)
    evil_dir.mkdir()
    gui_target = host / "BOLTPACK.PRG"
    shutil.copy2(gui, gui_target)

    replacement = b"archive data that is already published outside OUT\n" * 5
    original = b"the recorded old backup is kept outside OUT\n" * 4
    (host / "PAYLOAD.TXT").write_bytes(replacement)
    create = run([str(linux), "c", "ARCHIVE.BLT", "PAYLOAD.TXT"], host)
    (out / "archive-create.log").write_text(create.stdout, encoding="utf-8")
    if create.returncode != 0:
        raise RuntimeError("could not create journal-path fixture: " + str(out))

    target = evil_dir / "PAYLOAD.TXT"
    backup = evil_dir / "BRFAKE1"
    target.write_bytes(replacement)
    backup.write_bytes(original)
    journal = output_dir / "BPRECOVR.JRN"
    journal.write_bytes(make_journal(
        r"C:\OUT\..\EVIL\PAYLOAD.TXT",
        r"C:\OUT\..\EVIL\BPFAKE1",
        r"C:\OUT\..\EVIL\BRFAKE1",
        original, replacement))

    script_dir = out / "hatari-run"
    script_dir.mkdir()
    script = script_dir / "HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 4",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 3", "text OUT", "keypress 28",
        "sleep 20", "screenshot", "sleep 2", "quit", "",
    )), encoding="ascii")
    env = os.environ.copy()
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    env["PATH"] = str(Path(hatari).resolve().parent) + os.pathsep + env.get("PATH", "")
    command = [sys.executable, hconsole, str(script), "--exit", "--",
               "--machine", "ste", "--tos", str(tos), "--sound", "off",
               "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
               "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG"]
    log = (out / "hatari.log").open("w", encoding="utf-8")
    process = subprocess.Popen(command, cwd=script_dir, env=env, stdout=log,
                               stderr=subprocess.STDOUT, text=True,
                               start_new_session=True)
    try:
        process.wait(timeout=180)
    except subprocess.TimeoutExpired:
        import signal
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=30)
        log.close()
        raise RuntimeError("Hatari recovery-path run timed out: " + str(out))
    log.close()

    screenshot = script_dir / "grab0001.png"
    if (process.returncode != 0 or not screenshot.is_file() or
            target.read_bytes() != replacement or backup.read_bytes() != original or
            sorted(p.name for p in output_dir.iterdir()) != ["BPRECOVR.JRN"]):
        raise RuntimeError("malicious recovery journal changed or bypassed a path: " + str(out))

    status = run(["git", "status", "--short"], ROOT)
    diff = run(["git", "diff", "--binary", "--", "src/frontend/atari/atari_path.h",
                "src/frontend/atari_gui/main.c", "test/unit/test_atari_path.c"], ROOT)
    (out / "source-status.txt").write_text(status.stdout, encoding="utf-8")
    (out / "source-diff.patch").write_text(diff.stdout, encoding="utf-8")
    record = {
        "purpose": "prove Atari recovery rejects a checksummed journal whose target/stage/backup escape the selected output root",
        "result": "PASS: outside target and backup bytes were preserved; no output was extracted and the invalid journal remained",
        "hatari": run([hatari, "--version"], out).stdout.strip(),
        "tos_rom": tos.name,
        "tos_sha256": hashlib.sha256(tos.read_bytes()).hexdigest(),
        "gui_sha256": hashlib.sha256(gui.read_bytes()).hexdigest(),
        "archive_sha256": hashlib.sha256((host / "ARCHIVE.BLT").read_bytes()).hexdigest(),
        "attack_journal_sha256": hashlib.sha256(journal.read_bytes()).hexdigest(),
        "outside_target_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        "outside_backup_sha256": hashlib.sha256(backup.read_bytes()).hexdigest(),
        "screenshot": "hatari-run/grab0001.png",
        "source_revision": run(["git", "rev-parse", "HEAD"], ROOT).stdout.strip(),
        "source_status_sha256": hashlib.sha256(status.stdout.encode()).hexdigest(),
        "source_diff_sha256": hashlib.sha256(diff.stdout.encode()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "command": command,
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("PASS: Atari recovery refused a checksummed path-traversal journal; evidence:", out)


if __name__ == "__main__":
    main()
