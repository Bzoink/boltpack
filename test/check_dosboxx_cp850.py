#!/usr/bin/env python3
"""Verify DOS16 CP850 filename transfer on a DOSBox-X FAT12 floppy."""
import argparse
import datetime
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
EXPECTED_PATH = ROOT / "test/reference/interoperability/dos-cp850-fat12.json"
EXPECTED = json.loads(EXPECTED_PATH
                      .read_text(encoding="utf-8"))
CASE = EXPECTED["dosboxx_uppercase"]
PAYLOAD = CASE["payload"]["text"].encode("utf-8")
NAME = CASE["name_unicode"]


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


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
                                stderr=subprocess.STDOUT, text=True,
                                timeout=10, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable: " + str(exc)
    first = result.stdout.strip().splitlines()[0] if result.stdout.strip() else "unknown"
    return first if result.returncode == 0 else "%s (exit %d)" % (first, result.returncode)


def check_payload(path):
    data = path.read_bytes()
    expected = CASE["payload"]
    if len(data) != expected["bytes"] or \
            hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise RuntimeError("CP850 round-trip payload mismatch: " + str(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox-x", default="dosbox-x")
    parser.add_argument("--output", type=Path,
                        help="new directory for retained floppy image and logs")
    args = parser.parse_args()
    output_root = ROOT / "build"
    output_root.mkdir(exist_ok=True)
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(
        prefix="r02-dosboxx-cp850-", dir=output_root))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)

    source = out / "source"
    source.mkdir()
    (source / NAME).write_bytes(PAYLOAD)
    check_payload(source / NAME)
    linux = ROOT / "build/linux/boltpack"
    run([str(linux), "c", "--store", "OEM.BLT", NAME], cwd=source)

    image = out / "dos16-cp850-fat12.img"
    shutil.copyfile(ROOT / "build/dos16-cp850/bpmini.exe", out / "BPMINI.EXE")
    shutil.copyfile(source / "OEM.BLT", out / "OEM.BLT")
    shutil.copyfile(source / NAME, out / NAME)
    run(["mformat", "-i", str(image), "-C", "-f", "720", "::"])
    for name in ("BPMINI.EXE", "OEM.BLT", NAME):
        run(["mcopy", "-i", str(image), str(out / name), "::/" + name])
    run(["mmd", "-i", str(image), "::/OUT"])

    commands = [
        "set BP_TZ_OFFSET=0",
        "bpmini c DOS.BLT " + NAME + " > CREATE.LOG",
        "if errorlevel 1 echo FAIL > CREATE.BAD",
        "bpmini x OEM.BLT OUT > EXTRACT.LOG",
        "if errorlevel 1 echo PARTIAL > EXTRACT.BAD",
        "exit",
    ]
    # DOSBox-X passes batch command arguments as OEM bytes to the DOS program.
    (out / "CHECK.BAT").write_bytes(("\r\n".join(commands) + "\r\n").encode("cp850"))
    run(["mcopy", "-i", str(image), str(out / "CHECK.BAT"), "::/CHECK.BAT"])
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    emulator = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch",
                "-c", "imgmount a " + str(image) + " -t floppy", "-c", "a:",
                "-c", "CHECK.BAT", "-c", "exit"]
    output = run(emulator, env=env, timeout=60, cwd=out)
    (out / "dosboxx.log").write_bytes(output)
    version = subprocess.run([args.dosbox_x, "--version"], check=False,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "dosboxx-version.txt").write_bytes(version.stdout)

    dos_archive = out / "DOS.BLT"
    run(["mcopy", "-i", str(image), "::/DOS.BLT", str(dos_archive)])
    listing = run([str(linux), "l", str(dos_archive)]).decode("utf-8", "replace")
    (out / "linux-listing.txt").write_text(listing, encoding="utf-8")
    if NAME not in listing:
        raise RuntimeError("DOS16 CP850 archive member name was not UTF-8 Ø.TXT; evidence: " + str(out))
    dos_extract = out / "dos-created-extract"
    dos_extract.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(dos_extract), str(dos_archive)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if result.returncode not in (0, 1) or not (dos_extract / NAME).is_file():
        raise RuntimeError("Linux could not extract DOS16 CP850 member; evidence: " + str(out))
    check_payload(dos_extract / NAME)

    extracted = out / NAME
    run(["mcopy", "-i", str(image), "::/OUT/" + NAME, str(extracted)])
    check_payload(extracted)
    fat_bytes = image.read_bytes()
    # DOS canonicalizes argv through the CP850 uppercase table before opening
    # or creating the FAT 8.3 entry: Ø is byte 0x9D.
    short_name = bytes.fromhex(CASE["fat_short_name_hex"])
    if short_name not in fat_bytes:
        raise RuntimeError("FAT12 did not store CP850 Ø as byte 0x9D; evidence: " + str(out))
    result_text = "PASS: CP850 Ø.TXT archive creation, Linux extraction, and Linux-to-DOS16 extraction verified on DOSBox-X FAT12\n"
    (out / "result.txt").write_text(result_text, encoding="utf-8")
    try:
        locale_name = locale.setlocale(locale.LC_ALL, None)
    except locale.Error:
        locale_name = "unknown"
    status = subprocess.check_output(["git", "status", "--porcelain"],
                                     cwd=ROOT)
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"],
                                   cwd=ROOT)
    (out / "git-status.txt").write_bytes(status)
    (out / "git-diff.patch").write_bytes(diff)
    sources = (Path(__file__).resolve(), EXPECTED_PATH, ROOT / "Makefile",
               linux, ROOT / "build/dos16-cp850/bpmini.exe")
    record = {
        "case": EXPECTED["case"] + "; DOSBox-X CP850 uppercase transfer",
        "result": result_text.strip(),
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                             cwd=ROOT, text=True).strip(),
        "worktree_status": status.decode("utf-8", "replace").splitlines(),
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "environment": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "locale": locale_name,
            "timezone": list(time.tzname),
            "dosbox_x": command_version([args.dosbox_x, "--version"]),
            "mformat": command_version(["mformat", "--version"]),
        },
        "filesystem": "720 KiB FAT12 floppy image under DOSBox-X",
        "commands": {
            "linux_create": [str(linux), "c", "--store", "OEM.BLT", NAME],
            "guest_batch": commands,
            "emulator": emulator,
            "linux_list": [str(linux), "l", str(dos_archive)],
            "linux_extract": [str(linux), "x", "-o", str(dos_extract), str(dos_archive)],
        },
        "expected_manifest": {
            "path": EXPECTED_PATH.relative_to(ROOT).as_posix(),
            "size": EXPECTED_PATH.stat().st_size,
            "sha256": sha256(EXPECTED_PATH),
        },
        "source_sha256": {
            path.relative_to(ROOT).as_posix(): sha256(path)
            for path in sources if path.is_relative_to(ROOT)
        },
        "inputs": {
            "linux_cli_sha256": sha256(linux),
            "dos16_mini_sha256": sha256(ROOT / "build/dos16-cp850/bpmini.exe"),
        },
        "artifacts": artifact_index(out),
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("PASS: DOS16 CP850 Ø.TXT transfer under DOSBox-X FAT12; evidence:", out / "run.json")


if __name__ == "__main__":
    main()
