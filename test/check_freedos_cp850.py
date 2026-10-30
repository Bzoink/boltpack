#!/usr/bin/env python3
"""Verify DOS16 CP850 lowercase filenames on a disposable FreeDOS floppy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = json.loads((ROOT / "test/reference/interoperability/dos-cp850-fat12.json")
                      .read_text(encoding="utf-8"))
CASE = EXPECTED["freedos_lowercase"]
NAME = CASE["name_unicode"]
PAYLOAD = CASE["payload"]["text"].encode("utf-8")


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def check_payload(path):
    data = path.read_bytes()
    expected = CASE["payload"]
    if len(data) != expected["bytes"] or \
            hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise RuntimeError("FreeDOS CP850 payload mismatch: " + str(path))


def read_image_file(image, guest_path, host_path):
    run(["mcopy", "-o", "-i", str(image), "::/" + guest_path, str(host_path)])
    return host_path.read_bytes()


def write_image_file(image, host_path, guest_path):
    run(["mcopy", "-o", "-i", str(image), str(host_path), "::/" + guest_path])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-image", type=Path, required=True,
                        help="FreeDOS boot floppy image (1440 KiB x86BOOT.img)")
    parser.add_argument("--mode", type=Path, required=True,
                        help="MODE.COM from the FreeDOS mode package")
    parser.add_argument("--display", type=Path, required=True,
                        help="DISPLAY.EXE from the FreeDOS display package")
    parser.add_argument("--cpx", type=Path, required=True,
                        help="EGA.CPX from the FreeDOS mode package")
    parser.add_argument("--qemu", default="qemu-system-i386")
    parser.add_argument("--output", type=Path,
                        help="new directory for retained floppy image and logs")
    args = parser.parse_args()
    out = args.output.resolve() if args.output else \
        Path(tempfile.mkdtemp(prefix="boltpack-freedos-cp850-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)

    image = out / "freedos-cp850.img"
    shutil.copyfile(args.boot_image, image)
    linux = ROOT / "build/linux/boltpack"

    # Load FreeDOS's CP850 country tables at boot, then skip its disk installer
    # and run the exchange script from the boot floppy itself.
    config = out / "FDCONFIG.SYS"
    config.write_bytes(read_image_file(image, "FDCONFIG.SYS", config))
    raw = config.read_bytes()
    old_country = b"!COUNTRY=001,858:\\FREEDOS\\BIN\\COUNTRY.SYS"
    new_country = b"!COUNTRY=049,850,\\FREEDOS\\BIN\\COUNTRY.SYS"
    if old_country not in raw:
        raise RuntimeError("FreeDOS boot image has an unexpected country directive")
    config.write_bytes(raw.replace(old_country, new_country, 1))
    write_image_file(image, config, "FDCONFIG.SYS")

    autoexec = out / "FDAUTO.BAT"
    autoexec.write_bytes(read_image_file(image, "FDAUTO.BAT", autoexec))
    auto = autoexec.read_bytes()
    setup = b"if exist SETUP.BAT CALL SETUP.BAT BOOT\r\nif errorlevel 1 goto Done\r\n"
    runner = b"if exist TEST.BAT call TEST.BAT\r\n"
    if setup not in auto or b":Done" not in auto:
        raise RuntimeError("FreeDOS boot image has an unexpected startup script")
    auto = auto.replace(setup, b"", 1).replace(b":Done", runner + b":Done", 1)
    autoexec.write_bytes(auto)
    write_image_file(image, autoexec, "FDAUTO.BAT")

    source = out / "source"
    source.mkdir()
    (source / NAME).write_bytes(PAYLOAD)
    check_payload(source / NAME)
    run([str(linux), "c", "--store", "OEM.BLT", NAME], cwd=source)
    shutil.copyfile(ROOT / "build/dos16-cp850/bpmini.exe", out / "BPMINI.EXE")
    shutil.copyfile(source / "OEM.BLT", out / "OEM.BLT")
    shutil.copyfile(source / NAME, out / NAME)
    for host_name in ("BPMINI.EXE", "OEM.BLT", NAME):
        write_image_file(image, out / host_name, host_name)
    for source_path, guest_name in ((args.mode, "MODE.COM"),
                                    (args.display, "DISPLAY.EXE"),
                                    (args.cpx, "EGA.CPX")):
        write_image_file(image, source_path, guest_name)
    run(["mmd", "-i", str(image), "::/OUT"])

    commands = [
        "display CON=(EGA,437,1) > DISPLAY.LOG",
        "mode CON CP PREPARE=((850) EGA.CPX) > PREP.LOG",
        "mode CON CP SELECT=850 > SELECT.LOG",
        "chcp 850 > CHCPSET.LOG",
        "chcp > CHCP.LOG",
        "set BP_TZ_OFFSET=0",
        "dir " + NAME + " > DIR.LOG",
        "bpmini c DOS.BLT " + NAME + " > CREATE.LOG",
        "if errorlevel 1 echo FAIL > CREATE.BAD",
        "bpmini x OEM.BLT OUT > EXTRACT.LOG",
        "if errorlevel 1 echo PARTIAL > EXTRACT.BAD",
    ]
    commands.extend(["echo DONE > DONE.TXT", "fdapm poweroff"])
    batch = out / "TEST.BAT"
    batch.write_bytes(("\r\n".join(commands) + "\r\n").encode("cp850"))
    write_image_file(image, batch, "TEST.BAT")

    env = os.environ.copy()
    qemu = [args.qemu, "-machine", "pc", "-m", "16", "-boot", "order=a",
            "-drive", "file=" + str(image) + ",if=floppy,format=raw",
            "-display", "none", "-serial", "null", "-monitor", "none",
            "-no-reboot"]
    result = subprocess.run(qemu, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, cwd=out, timeout=45)
    (out / "qemu.log").write_bytes(result.stdout)
    for guest_log in ("DISPLAY.LOG", "PREP.LOG", "SELECT.LOG", "CHCPSET.LOG", "CHCP.LOG",
                      "DIR.LOG", "CREATE.LOG", "CREATE.BAD", "EXTRACT.LOG",
                      "EXTRACT.BAD"):
        try:
            read_image_file(image, guest_log, out / guest_log)
        except subprocess.CalledProcessError:
            pass

    marker = out / "DONE.TXT"
    read_image_file(image, "DONE.TXT", marker)
    done = marker.read_bytes()
    if b"DONE" not in done:
        raise RuntimeError("FreeDOS exchange did not reach its completion marker; evidence: " + str(out))
    chcp_log = (out / "CHCP.LOG").read_text(encoding="ascii", errors="replace")
    dir_log = (out / "DIR.LOG").read_bytes().decode("cp850", "replace")
    create_log = (out / "CREATE.LOG").read_bytes().decode("cp850", "replace")
    if "current codepage is 850" not in chcp_log or "1 file(s)" not in dir_log or \
            "33 bytes" not in dir_log or "Ø" not in dir_log or \
            "ø.TXT" not in create_log or "Archive: DOS.BLT" not in create_log:
        raise RuntimeError("FreeDOS did not confirm CP850 lowercase lookup/create; evidence: " + str(out))
    dos_archive = out / "DOS.BLT"
    try:
        run(["mcopy", "-i", str(image), "::/DOS.BLT", str(dos_archive)])
    except subprocess.CalledProcessError as exc:
        raise RuntimeError("FreeDOS did not create DOS.BLT; guest codepage and lookup logs retained in " + str(out)) from exc
    listing = run([str(linux), "l", str(dos_archive)]).decode("utf-8", "replace")
    (out / "linux-listing.txt").write_text(listing, encoding="utf-8")
    if NAME not in listing:
        raise RuntimeError("FreeDOS archive did not preserve lowercase CP850 ø.TXT; evidence: " + str(out))
    dos_extract = out / "dos-created-extract"
    dos_extract.mkdir()
    extracted_result = subprocess.run([str(linux), "x", "-o", str(dos_extract), str(dos_archive)],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(extracted_result.stdout)
    if extracted_result.returncode not in (0, 1) or not (dos_extract / NAME).is_file():
        raise RuntimeError("Linux could not extract FreeDOS CP850 member; evidence: " + str(out))
    check_payload(dos_extract / NAME)

    extracted = out / "linux-to-freedos-ø.TXT"
    run(["mcopy", "-i", str(image), "::/OUT/" + NAME, str(extracted)])
    check_payload(extracted)
    if bytes.fromhex(CASE["fat_short_name_hex"]) not in image.read_bytes():
        raise RuntimeError("FreeDOS FAT12 short name did not uppercase CP850 ø to byte 0x9D")
    (out / "result.txt").write_text(
        "PASS: FreeDOS CP850 lowercase ø.TXT archive creation and extraction in both directions\n",
        encoding="utf-8")
    print("PASS: DOS16 CP850 lowercase ø.TXT transfer under FreeDOS; evidence:", out)


if __name__ == "__main__":
    main()
