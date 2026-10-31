#!/usr/bin/env python3
"""Verify the Atari GEM GUI refuses an unrepresentable UTF-8 archive name."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = ROOT / "test/reference/interoperability/atari-gui-unicode-refusal.json"
MANIFEST = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


def run(command, cwd, env=None, timeout=180):
    return subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", type=Path, default=ROOT / "build/atari/boltpack.prg")
    parser.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    args = parser.parse_args()

    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-gui-unicode-", dir=output_root))
    host = out / "host"
    host.mkdir()
    source = out / "source"
    source.mkdir()

    payloads = {}
    for item in MANIFEST["entries"]:
        payload = item["content"].encode("utf-8")
        if hashlib.sha256(payload).hexdigest() != item["sha256"]:
            raise RuntimeError("manifest payload hash mismatch: " + item["name"])
        (source / item["name"]).write_bytes(payload)
        payloads[item["name"]] = payload

    archive = host / MANIFEST["archive"]
    create_command = ([str(linux), "c", str(archive)] +
                      [item["name"] for item in MANIFEST["entries"]])
    create = run(create_command, source)
    (out / "linux-create.log").write_text(create.stdout, encoding="utf-8")
    if create.returncode != 0:
        raise RuntimeError("Linux could not create Unicode fixture: " + str(out))
    integrity = run([str(linux), "t", str(archive)], out)
    (out / "linux-integrity.log").write_text(integrity.stdout, encoding="utf-8")
    if integrity.returncode != 0:
        raise RuntimeError("Linux rejected its Unicode fixture: " + str(out))
    info = run([str(linux), "i", str(archive)], out)
    (out / "linux-info.log").write_text(info.stdout, encoding="utf-8")
    if "Format version: 6" not in info.stdout:
        raise RuntimeError("fixture is not a format-6 archive: " + str(out))

    shutil.copy2(gui, host / "BOLTPACK.PRG")
    destination = host / "OUT"
    destination.mkdir()
    script = out / "HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text " + MANIFEST["archive"], "keypress 28",
        "sleep 4",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 3", "text OUT", "keypress 28",
        "sleep 6", "screenshot",
        "sleep 2", "keypress 28", "sleep 1", "quit", "",
    )), encoding="ascii")
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    command = [
        sys.executable, str(hconsole), str(script), "--exit", "--",
        "--machine", "ste", "--tos", str(tos), "--sound", "off",
        "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
        "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG",
    ]
    guest = run(command, out, env=env, timeout=150)
    (out / "hatari.log").write_text(guest.stdout, encoding="utf-8")
    if guest.returncode != 0:
        raise RuntimeError("Hatari GUI run failed: " + str(out))
    screenshot = out / "grab0001.png"
    if not screenshot.is_file():
        raise RuntimeError("GUI refusal screenshot is missing: " + str(out))
    published = sorted(path.relative_to(destination).as_posix()
                       for path in destination.rglob("*") if path.is_file())
    if published:
        raise RuntimeError("Atari GUI published entries despite an unrepresentable name: " +
                           repr(published) + "; evidence: " + str(out))
    residue = sorted(path.name for path in destination.iterdir())
    if residue:
        raise RuntimeError("Atari GUI left transaction files after refusal: " +
                           repr(residue) + "; evidence: " + str(out))

    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                              check=True, capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                                check=True, capture_output=True, text=True).stdout.strip())
    record = {
        "case": MANIFEST["case"],
        "expected_manifest": str(MANIFEST_PATH.relative_to(ROOT)),
        "expected_manifest_sha256": hashlib.sha256(MANIFEST_PATH.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "makefile_sha256": hashlib.sha256((ROOT / "Makefile").read_bytes()).hexdigest(),
        "tested_revision": revision,
        "worktree_dirty": dirty,
        "tos_rom": tos.name,
        "tos_image_sha256": hashlib.sha256(tos.read_bytes()).hexdigest(),
        "hatari_version": next((line for line in guest.stdout.splitlines()
                                 if "Hatari v" in line), "unknown"),
        "gui_program_sha256": hashlib.sha256(gui.read_bytes()).hexdigest(),
        "linux_cli_sha256": hashlib.sha256(linux.read_bytes()).hexdigest(),
        "linux_create_command": create_command,
        "linux_integrity_command": [str(linux), "t", str(archive)],
        "linux_integrity_returncode": integrity.returncode,
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "entry_payload_sha256": {name: hashlib.sha256(data).hexdigest()
                                  for name, data in payloads.items()},
        "filesystem": "Hatari GEMDOS host directory",
        "destination_files_after_refusal": published,
        "destination_residue_after_refusal": residue,
        "screenshot": screenshot.name,
        "screenshot_sha256": hashlib.sha256(screenshot.read_bytes()).hexdigest(),
        "hatari_log_sha256": hashlib.sha256((out / "hatari.log").read_bytes()).hexdigest(),
        "hatari_command": command,
        "result": "PASS: Linux format-6 archive integrity passed; Atari GUI refused the archive before publishing the representable sibling or unrepresentable UTF-8 entry",
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("Atari GEM unrepresentable UTF-8 name refusal passed on " + tos.name)
    print("Destination remained empty; screenshot and run record: " + str(out))


if __name__ == "__main__":
    main()
