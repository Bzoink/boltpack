#!/usr/bin/env python3
"""Verify DOS16 CP850 filename transfer on a DOSBox-X FAT12 floppy."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PAYLOAD = b"CP850 filename on FAT12\n"
NAME = "\u00d8.TXT"


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def check_payload(path):
    data = path.read_bytes()
    if data != PAYLOAD or hashlib.sha256(data).hexdigest() != \
            "45805b1de39b3b51875eefb87b6518424963391b129af64e17fb183fea6b3e56":
        raise RuntimeError("CP850 round-trip payload mismatch: " + str(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox-x", default="dosbox-x")
    parser.add_argument("--output", type=Path,
                        help="new directory for retained floppy image and logs")
    args = parser.parse_args()
    out = args.output.resolve() if args.output else \
        Path(tempfile.mkdtemp(prefix="boltpack-dosboxx-cp850-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)

    source = out / "source"
    source.mkdir()
    (source / NAME).write_bytes(PAYLOAD)
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
    short_name = b"\x9d       TXT"
    if short_name not in fat_bytes:
        raise RuntimeError("FAT12 did not store CP850 Ø as byte 0x9D; evidence: " + str(out))
    (out / "result.txt").write_text(
        "PASS: CP850 Ø.TXT archive creation, Linux extraction, and Linux-to-DOS16 extraction verified on DOSBox-X FAT12\n",
        encoding="utf-8")
    print("PASS: DOS16 CP850 Ø.TXT transfer under DOSBox-X FAT12; evidence:", out)


if __name__ == "__main__":
    main()
