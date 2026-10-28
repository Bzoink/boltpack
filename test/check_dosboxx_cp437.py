#!/usr/bin/env python3
"""Verify DOS16 CP437 filename extraction on a DOSBox-X FAT12 floppy."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox-x", default="dosbox-x")
    parser.add_argument("--output", type=Path,
                        help="new directory for retained floppy image and logs")
    args = parser.parse_args()
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="boltpack-dosboxx-cp437-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    image = out / "dos16-fat12.img"
    source = out / "source"
    source.mkdir()
    (source / "café.txt").write_bytes(b"CP437 filename on FAT12\n")
    os.utime(source / "café.txt", (951786000, 951786000))
    (source / "SRC.TXT").write_bytes(b"DOS16 FAT12 to Linux\n")
    shutil.copyfile(ROOT / "build/dos16/bpmini.exe", out / "BPMINI.EXE")
    linux = ROOT / "build/linux/boltpack"
    run([str(linux), "c", "--store", "OEM.BLT", "café.txt"], cwd=source)
    shutil.copyfile(source / "OEM.BLT", out / "OEM.BLT")
    shutil.copyfile(source / "SRC.TXT", out / "SRC.TXT")
    run(["mformat", "-i", str(image), "-C", "-f", "720", "::"])
    for name in ("BPMINI.EXE", "OEM.BLT", "SRC.TXT"):
        run(["mcopy", "-i", str(image), str(out / name), "::/" + name])
    run(["mmd", "-i", str(image), "::/OUT"])

    commands = [
        "imgmount a " + str(image) + " -t floppy",
        "a:",
        "set BP_TZ_OFFSET=0",
        "bpmini c DOS.BLT SRC.TXT > CREATE.LOG",
        "if errorlevel 1 echo FAIL > CREATE.BAD",
        "bpmini x OEM.BLT OUT > X.LOG",
        "if errorlevel 1 echo PARTIAL > X.BAD",
        "exit",
    ]
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    emulator = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch"]
    for command in commands:
        emulator.extend(["-c", command])
    output = run(emulator, env=env, timeout=40)
    (out / "dosboxx.log").write_bytes(output)
    version = subprocess.run([args.dosbox_x, "--version"], check=False,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT).stdout.decode("utf-8", "replace")
    (out / "dosboxx-version.txt").write_text(version)

    extracted = out / "cafe.txt"
    run(["mcopy", "-i", str(image), "::/OUT/CAFé.TXT", str(extracted)])
    if extracted.read_bytes() != b"CP437 filename on FAT12\n":
        raise RuntimeError("DOS16 FAT12 CP437 extraction bytes differ; evidence: " + str(out))
    dos_archive = out / "dos-created.blt"
    run(["mcopy", "-i", str(image), "::/DOS.BLT", str(dos_archive)])
    dos_listing = run([str(linux), "l", str(dos_archive)]).decode("utf-8", "replace")
    (out / "linux-listing.txt").write_text(dos_listing)
    extracted_dos = out / "linux-extract"
    extracted_dos.mkdir()
    linux_extract = subprocess.run([str(linux), "x", "-o", str(extracted_dos), str(dos_archive)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(linux_extract.stdout)
    if (linux_extract.returncode != 1 or b"not restored" not in linux_extract.stdout or
            "SRC.TXT" not in dos_listing or
            (extracted_dos / "SRC.TXT").read_bytes() != b"DOS16 FAT12 to Linux\n"):
        raise RuntimeError("Linux could not verify DOS16 FAT12-created archive; evidence: " + str(out))
    listing = run(["mdir", "-i", str(image), "::/OUT"]).decode("utf-8", "replace")
    (out / "fat-listing.txt").write_text(listing)
    if "CAFé" not in listing or "2000-02-29" not in listing or "1:00" not in listing:
        raise RuntimeError("FAT12 filename or restored timestamp differs; evidence: " + str(out))

    partial = subprocess.run(["mdir", "-i", str(image), "::/X.BAD"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if partial.returncode == 0:
        run(["mcopy", "-i", str(image), "::/X.BAD", str(out / "X.BAD")])
        status = "partial reported after successful FAT12 byte extraction"
    else:
        status = "complete"
    if (out / "CREATE.BAD").exists():
        raise RuntimeError("DOS16 create failed on FAT12; evidence: " + str(out))
    (out / "result.txt").write_text("PASS: CP437 extraction and DOS16-to-Linux archive creation verified on DOSBox-X FAT12\nresult=" + status + "\n")
    print("PASS: DOS16 CP437 extraction and DOS16-to-Linux creation under DOSBox-X FAT12;", status + "; evidence:", out)


if __name__ == "__main__":
    main()
