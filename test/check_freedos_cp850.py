#!/usr/bin/env python3
"""Verify DOS16 CP850 lowercase filenames on a disposable FreeDOS floppy."""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = json.loads((ROOT / "test/reference/interoperability/dos-cp850-fat12.json")
                      .read_text(encoding="utf-8"))
CASE = EXPECTED["freedos_lowercase"]
DIR_CASE = EXPECTED["dos_directory_metadata"]
NAME = CASE["name_unicode"]
PAYLOAD = CASE["payload"]["text"].encode("utf-8")
DIR_PAYLOAD = DIR_CASE["payload"]["text"].encode("utf-8")


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def check_payload(path):
    data = path.read_bytes()
    expected = CASE["payload"]
    if len(data) != expected["bytes"] or \
            hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise RuntimeError("FreeDOS CP850 payload mismatch: " + str(path))


def fat_attribute_letters(output):
    line = output.decode("ascii", "replace").splitlines()[0]
    return set(line.split("::", 1)[0].replace(" ", "").upper())


def read_image_file(image, guest_path, host_path):
    run(["mcopy", "-o", "-i", str(image), "::/" + guest_path, str(host_path)])
    return host_path.read_bytes()


def write_image_file(image, host_path, guest_path):
    run(["mcopy", "-o", "-i", str(image), str(host_path), "::/" + guest_path])


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tool_version(command):
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    return {"command": command, "returncode": result.returncode,
            "output": result.stdout.strip()}


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
    parser.add_argument("--crash-exe", type=Path, required=True,
                        help="test-only DOS16 executable that exits after target-to-backup rename")
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
    tree_source = out / "linux-tree-source"
    tree_root = tree_source / DIR_CASE["member"]
    for relative in DIR_CASE["directories"]:
        (tree_root / relative).mkdir(parents=True, exist_ok=True)
    tree_payload_path = tree_root / DIR_CASE["file"]
    tree_payload_path.write_bytes(DIR_PAYLOAD)
    fixed_mtime = DIR_CASE["source_mtime_unix"]
    for directory in reversed([tree_root] +
                              [tree_root / name for name in DIR_CASE["directories"]]):
        os.utime(directory, (fixed_mtime, fixed_mtime))
    run([str(linux), "c", "--store", "LINUXTR.BLT", DIR_CASE["member"]],
        cwd=tree_source, env={**os.environ, "BP_TZ_OFFSET": "0"})
    failure_source = out / "disk-full-source"
    failure_source.mkdir()
    failure_payload = random.Random(0xB017).randbytes(380 * 1024)
    failure_file = failure_source / "BIG.DAT"
    failure_file.write_bytes(failure_payload)
    failure_archive = source / "DOSFAIL.BLT"
    failure_archive.write_bytes((source / "OEM.BLT").read_bytes())
    crash_archive = source / "CRASH.BLT"
    crash_archive.write_bytes((source / "OEM.BLT").read_bytes())
    recovery_backup = source / "BK0001.BAK"
    recovery_backup.write_bytes((source / "OEM.BLT").read_bytes())
    existing_target = source / "EXIST.BLT"
    existing_target.write_bytes((source / "OEM.BLT").read_bytes())
    refusal_backup = source / "BK0002.BAK"
    refusal_backup.write_bytes((tree_source / "LINUXTR.BLT").read_bytes())
    corrupt_backup = source / "BK0003.BAK"
    corrupt_backup.write_bytes(b"not a valid BoltPack archive\r\n")
    shutil.copyfile(ROOT / "build/dos16-cp850/bpmini.exe", out / "BPMINI.EXE")
    shutil.copyfile(args.crash_exe, out / "BPCRASH.EXE")
    shutil.copyfile(source / "OEM.BLT", out / "OEM.BLT")
    shutil.copyfile(source / NAME, out / NAME)
    shutil.copyfile(tree_source / "LINUXTR.BLT", out / "LINUXTR.BLT")
    for host_name in ("BPMINI.EXE", "BPCRASH.EXE", "OEM.BLT", NAME, "LINUXTR.BLT"):
        write_image_file(image, out / host_name, host_name)
    write_image_file(image, failure_archive, "DOSFAIL.BLT")
    write_image_file(image, failure_file, "BIG.DAT")
    write_image_file(image, crash_archive, "CRASH.BLT")
    write_image_file(image, recovery_backup, "BK0001.BAK")
    write_image_file(image, existing_target, "EXIST.BLT")
    write_image_file(image, refusal_backup, "BK0002.BAK")
    write_image_file(image, corrupt_backup, "BK0003.BAK")
    for source_path, guest_name in ((args.mode, "MODE.COM"),
                                    (args.display, "DISPLAY.EXE"),
                                    (args.cpx, "EGA.CPX")):
        write_image_file(image, source_path, guest_name)
    for directory in ("SOURCE", "SOURCE/EMPTY", "SOURCE/NEST",
                      "OUT", "OUTLINUX", "OUTDOS"):
        run(["mmd", "-i", str(image), "::/" + directory])
    guest_tree_payload = out / "NESTED.TXT"
    guest_tree_payload.write_bytes(DIR_PAYLOAD)
    run(["mcopy", "-i", str(image), str(guest_tree_payload),
         "::/SOURCE/" + DIR_CASE["file"]])
    run(["mattrib", "-i", str(image), "+h", "+s", "::/SOURCE"])
    run(["mattrib", "-i", str(image), "+h", "::/SOURCE/EMPTY"])
    free_listing = run(["mdir", "-i", str(image), "::/"]).decode("ascii", "replace")
    free_match = re.search(r"([0-9, ]+) bytes free", free_listing)
    if free_match is None:
        raise RuntimeError("Could not determine remaining FreeDOS FAT12 space")
    remaining_bytes = int(free_match.group(1).replace(",", "").replace(" ", ""))
    if not 32 * 1024 < remaining_bytes < len(failure_payload):
        raise RuntimeError("FAT12 fixture does not leave a bounded disk-full margin: " +
                           str(remaining_bytes))

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
        "bpmini x LINUXTR.BLT OUTLINUX > LINTREE.LOG 2> LINTERR.LOG",
        "if errorlevel 1 echo PARTIAL > LINTREE.BAD",
        "bpmini c DOS-TREE.BLT SOURCE > TREECR.LOG",
        "if errorlevel 1 echo FAIL > TREECR.BAD",
        "bpmini x DOS-TREE.BLT OUTDOS > DOSTREE.LOG 2> DOSTERR.LOG",
        "if errorlevel 1 echo PARTIAL > DOSTREE.BAD",
        "bpmini c DOSFAIL.BLT BIG.DAT > FAILPUB.LOG",
        "if errorlevel 1 echo EXPECTED > FAILPUB.OK",
        "bpmini r RECOVER.BLT BK0001.BAK > RECOVER.LOG",
        "if errorlevel 1 echo FAIL > RECOVER.BAD",
        "bpmini r EXIST.BLT BK0002.BAK > REFUSE.LOG",
        "if errorlevel 1 echo EXPECTED > REFUSE.OK",
        "bpmini r CORRUPT.BLT BK0003.BAK > CORRUPT.LOG",
        "if errorlevel 1 echo EXPECTED > CORRUPT.OK",
        "BPCRASH c CRASH.BLT SOURCE > CRASH.LOG",
        "if errorlevel 1 echo EXPECTED > CRASH.OK",
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
    crash_marker = out / "CRASH.OK"
    read_image_file(image, "CRASH.OK", crash_marker)
    if crash_marker.read_text(encoding="ascii").strip() != "EXPECTED":
        raise RuntimeError("DOS16 crash hook did not exit after the backup rename")
    crash_state = run(["mdir", "-i", str(image), "::/"]).decode("ascii", "replace")
    crash_stages = re.findall(r"\b(BPK[0-9A-Z]{4})\s+TMP\b", crash_state)
    crash_backups = re.findall(r"\b(BK[0-9A-Z]{4})\s+BAK\b", crash_state)
    if crash_stages != ["BPK0001"] or set(crash_backups) != {"BK0001", "BK0002", "BK0003"}:
        raise RuntimeError("Injected DOS16 interruption did not leave its paired stage and backup")
    crash_backup = out / "BK0001.BAK"
    crash_stage = out / "BPK0001.TMP"
    read_image_file(image, "BK0001.BAK", crash_backup)
    read_image_file(image, "BPK0001.TMP", crash_stage)
    if crash_backup.read_bytes() != crash_archive.read_bytes():
        raise RuntimeError("DOS16 crash-window backup differs from the old archive")
    run([str(linux), "t", str(crash_backup)])
    run([str(linux), "t", str(crash_stage)])
    missing_target = subprocess.run(["mcopy", "-i", str(image), "::/CRASH.BLT",
                                     str(out / "unexpected-crash-target")],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if missing_target.returncode == 0:
        raise RuntimeError("DOS16 crash hook left the target published before recovery")
    recovery_commands = [
        "set BP_TZ_OFFSET=0",
        "bpmini r CRASH.BLT BK0001.BAK > CRASHREC.LOG",
        "if errorlevel 1 echo FAIL > CRASHREC.BAD",
        "echo DONE2 > DONE.TXT",
        "fdapm poweroff",
    ]
    recovery_batch = out / "RECOVER.BAT"
    recovery_batch.write_bytes(("\r\n".join(recovery_commands) + "\r\n").encode("cp850"))
    write_image_file(image, recovery_batch, "TEST.BAT")
    run(["mdel", "-i", str(image), "::/DONE.TXT"])
    recovery_result = subprocess.run(qemu, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, env=env,
                                     cwd=out, timeout=45)
    (out / "qemu-recovery.log").write_bytes(recovery_result.stdout)
    for guest_log in ("DISPLAY.LOG", "PREP.LOG", "SELECT.LOG", "CHCPSET.LOG", "CHCP.LOG",
                      "DIR.LOG", "CREATE.LOG", "CREATE.BAD", "EXTRACT.LOG",
                      "EXTRACT.BAD", "LINTREE.LOG", "LINTREE.BAD",
                      "TREECR.LOG", "TREECR.BAD", "DOSTREE.LOG",
                      "DOSTREE.BAD", "LINTERR.LOG", "DOSTERR.LOG",
                      "FAILPUB.LOG", "FAILPUB.OK", "RECOVER.LOG",
                      "RECOVER.BAD", "REFUSE.LOG", "REFUSE.OK",
                      "CORRUPT.LOG", "CORRUPT.OK",
                      "CRASH.LOG", "CRASH.OK", "CRASHREC.LOG",
                      "CRASHREC.BAD"):
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
    tree_outputs = {}
    for guest_root, label in (("OUTLINUX", "linux-to-dos"),
                              ("OUTDOS", "dos-to-dos")):
        target = out / (label + "-nested.txt")
        guest_member = DIR_CASE["member"] if guest_root == "OUTLINUX" else "SOURCE"
        guest_file = guest_root + "/" + guest_member + "/" + DIR_CASE["file"]
        read_image_file(image, guest_file, target)
        if target.read_bytes() != DIR_PAYLOAD:
            raise RuntimeError("FreeDOS directory-tree extraction payload mismatch: " + label)
        for relative in DIR_CASE["directories"]:
            listing = run(["mdir", "-i", str(image),
                           "::/" + guest_root + "/" + guest_member + "/" + relative])
            if b"Directory for" not in listing:
                raise RuntimeError("FreeDOS did not create expected directory: " + relative)
        tree_outputs[label] = sha256(target)
    partial_markers = {}
    for guest_log in ("LINTREE.BAD", "DOSTREE.BAD"):
        marker_path = out / guest_log
        read_image_file(image, guest_log, marker_path)
        partial_markers[guest_log] = marker_path.read_bytes().decode("ascii", "replace").strip()
        if "PARTIAL" not in partial_markers[guest_log]:
            raise RuntimeError("FreeDOS directory metadata loss did not report partial: " + guest_log)
    directory_attrs = run(["mattrib", "-i", str(image), "::/OUTDOS/SOURCE"])
    empty_attrs = run(["mattrib", "-i", str(image), "::/OUTDOS/SOURCE/EMPTY"])
    if not set(DIR_CASE["source_directory_attributes"]).issubset(
            fat_attribute_letters(directory_attrs)):
        raise RuntimeError("FreeDOS did not restore source directory FAT attributes: " +
                           directory_attrs.decode("ascii", "replace"))
    if not set(DIR_CASE["empty_directory_attributes"]).issubset(
            fat_attribute_letters(empty_attrs)):
        raise RuntimeError("FreeDOS did not restore empty-directory FAT attributes: " +
                           empty_attrs.decode("ascii", "replace"))
    dos_tree_archive = out / "DOS-TREE.BLT"
    read_image_file(image, "DOS-TREE.BLT", dos_tree_archive)
    run([str(linux), "t", str(dos_tree_archive)])
    dos_tree_listing = run([str(linux), "l", str(dos_tree_archive)]).decode("utf-8", "replace")
    if "SOURCE/" not in dos_tree_listing or DIR_CASE["file"] not in dos_tree_listing:
        raise RuntimeError("DOS16-created archive omitted directory-tree members")
    linux_tree_out = out / "linux-extract-dos-tree"
    linux_tree_out.mkdir()
    tree_extract = subprocess.run([str(linux), "x", "-o", str(linux_tree_out),
                                   str(dos_tree_archive)], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
    (out / "linux-tree-extract.log").write_bytes(tree_extract.stdout)
    linux_tree_file = linux_tree_out / "SOURCE" / DIR_CASE["file"]
    if tree_extract.returncode not in (0, 1) or not linux_tree_file.is_file() or \
            linux_tree_file.read_bytes() != DIR_PAYLOAD:
        raise RuntimeError("Linux could not recover the DOS16-created directory tree")
    tree_outputs["dos-to-linux"] = sha256(linux_tree_file)
    failure_marker = out / "FAILPUB.OK"
    read_image_file(image, "FAILPUB.OK", failure_marker)
    if failure_marker.read_text(encoding="ascii").strip() != "EXPECTED":
        raise RuntimeError("FAT12 low-space replacement did not fail during staging")
    preserved_archive = out / "DOSFAIL.BLT"
    read_image_file(image, "DOSFAIL.BLT", preserved_archive)
    expected_archive_bytes = failure_archive.read_bytes()
    if preserved_archive.read_bytes() != expected_archive_bytes:
        raise RuntimeError("DOS16 low-space replacement changed the previous archive")
    recovered_archive = out / "RECOVER.BLT"
    read_image_file(image, "RECOVER.BLT", recovered_archive)
    if recovered_archive.read_bytes() != (source / "OEM.BLT").read_bytes():
        raise RuntimeError("DOS16 recovery changed the selected backup archive")
    run([str(linux), "t", str(recovered_archive)])
    refusal_marker = out / "REFUSE.OK"
    read_image_file(image, "REFUSE.OK", refusal_marker)
    if refusal_marker.read_text(encoding="ascii").strip() != "EXPECTED":
        raise RuntimeError("DOS16 recovery did not refuse an existing target")
    existing_after = out / "EXIST.BLT"
    refusal_after = out / "BK0002.BAK"
    read_image_file(image, "EXIST.BLT", existing_after)
    read_image_file(image, "BK0002.BAK", refusal_after)
    if existing_after.read_bytes() != existing_target.read_bytes() or \
            refusal_after.read_bytes() != refusal_backup.read_bytes():
        raise RuntimeError("DOS16 recovery refusal changed the target or backup")
    corrupt_marker = out / "CORRUPT.OK"
    read_image_file(image, "CORRUPT.OK", corrupt_marker)
    if corrupt_marker.read_text(encoding="ascii").strip() != "EXPECTED":
        raise RuntimeError("DOS16 recovery did not reject an invalid backup")
    corrupt_after = out / "BK0003.BAK"
    read_image_file(image, "BK0003.BAK", corrupt_after)
    corrupt_target = subprocess.run(["mcopy", "-i", str(image), "::/CORRUPT.BLT",
                                     str(out / "unexpected-corrupt-target")],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if corrupt_target.returncode == 0 or corrupt_after.read_bytes() != corrupt_backup.read_bytes():
        raise RuntimeError("DOS16 corrupt-backup refusal changed or restored invalid data")
    crash_marker = out / "CRASH.OK"
    read_image_file(image, "CRASH.OK", crash_marker)
    if crash_marker.read_text(encoding="ascii").strip() != "EXPECTED":
        raise RuntimeError("DOS16 crash hook did not stop after moving the old target")
    crash_recovered = out / "CRASH.BLT"
    read_image_file(image, "CRASH.BLT", crash_recovered)
    if crash_recovered.read_bytes() != crash_archive.read_bytes():
        raise RuntimeError("DOS16 recovery after the backup rename changed the old archive")
    run([str(linux), "t", str(crash_recovered)])
    root_listing = run(["mdir", "-i", str(image), "::/"]).decode("ascii", "replace")
    stages = re.findall(r"\b(BPK[0-9A-Z]{4})\s+TMP\b", root_listing)
    backups = re.findall(r"\b(BK[0-9A-Z]{4})\s+BAK\b", root_listing)
    if stages or set(backups) != {"BK0002", "BK0003"}:
        raise RuntimeError("DOS16 left unexpected stages or backups after publication/recovery")
    if bytes.fromhex(CASE["fat_short_name_hex"]) not in image.read_bytes():
        raise RuntimeError("FreeDOS FAT12 short name did not uppercase CP850 ø to byte 0x9D")
    (out / "result.txt").write_text(
        "PASS: FreeDOS CP850, directory metadata, low-space publication safety, and explicit backup recovery; directory times report partial\n",
        encoding="utf-8")
    status = run(["git", "status", "--short"], cwd=ROOT)
    relevant_diff = run(["git", "diff", "--binary", "--", "Makefile",
                         "src/core/mini/archiver.c", "src/core/mini/compress.c",
                         "src/core/bp_codepage.c", "src/frontend/dos16/main.c",
                         "test/check_freedos_cp850.py",
                         "test/reference/interoperability/dos-cp850-fat12.json"],
                        cwd=ROOT)
    (out / "source-status.txt").write_bytes(status)
    (out / "source-diff.patch").write_bytes(relevant_diff)
    revision = run(["git", "rev-parse", "HEAD"], cwd=ROOT).decode().strip()
    expected_path = ROOT / "test/reference/interoperability/dos-cp850-fat12.json"
    run_record = {
        "purpose": "FreeDOS 1.4 CP850, DOS16 directory metadata, low-space replacement, and explicit backup recovery interoperability",
        "result": "PASS: lowercase ø.TXT, nested directory trees, FAT directory flags, low-space replacement preservation, and explicit backup recovery; unsupported directory times report partial",
        "date": "2026-10-30",
        "source_revision": revision,
        "worktree_dirty": bool(status.strip()),
        "source_status_sha256": hashlib.sha256(status).hexdigest(),
        "relevant_source_diff_sha256": hashlib.sha256(relevant_diff).hexdigest(),
        "runner_sha256": sha256(Path(__file__)),
        "expected_manifest": str(expected_path.relative_to(ROOT)),
        "expected_manifest_sha256": sha256(expected_path),
        "host": platform.platform(),
        "locale": {name: os.environ.get(name) for name in
                   ("LANG", "LC_ALL", "LC_CTYPE", "TZ")},
        "filesystem": "1.44 MiB FreeDOS FAT12 floppy image",
        "emulator_command": qemu,
        "guest_commands": commands,
        "tools": [tool_version([args.qemu, "--version"]),
                  tool_version(["mcopy", "--version"])],
        "assets": {
            "freedos_boot_image_sha256": sha256(args.boot_image),
            "mode_com_sha256": sha256(args.mode),
            "display_exe_sha256": sha256(args.display),
            "ega_cpx_sha256": sha256(args.cpx),
        },
        "artifacts": {
            "modified_boot_floppy_sha256": sha256(image),
            "linux_created_archive_sha256": sha256(source / "OEM.BLT"),
            "freedos_created_archive_sha256": sha256(dos_archive),
            "linux_created_tree_archive_sha256": sha256(tree_source / "LINUXTR.BLT"),
            "freedos_created_tree_archive_sha256": sha256(dos_tree_archive),
            "payload_sha256": hashlib.sha256(PAYLOAD).hexdigest(),
            "linux_extracted_payload_sha256": sha256(dos_extract / NAME),
            "dos_extracted_payload_sha256": sha256(extracted),
            "directory_tree_payload_sha256": hashlib.sha256(DIR_PAYLOAD).hexdigest(),
            "directory_tree_transfer_payload_sha256": tree_outputs,
            "low_space_source_payload_sha256": hashlib.sha256(failure_payload).hexdigest(),
            "existing_archive_preserved_after_low_space_failure": sha256(preserved_archive),
            "recovered_archive_sha256": sha256(recovered_archive),
            "existing_target_after_refused_recovery_sha256": sha256(existing_after),
            "backup_after_refused_recovery_sha256": sha256(refusal_after),
            "corrupt_backup_after_refused_recovery_sha256": sha256(corrupt_after),
            "crash_window_recovered_archive_sha256": sha256(crash_recovered),
        },
        "verified": {
            "active_codepage": 850,
            "lowercase_dos_lookup": NAME,
            "fat_short_name_hex": CASE["fat_short_name_hex"],
            "linux_read_dos_archive": True,
            "dos_extract_linux_archive": True,
            "payloads_exact": True,
            "nested_directory_transfers": ["Linux to FreeDOS", "FreeDOS to Linux", "FreeDOS to FreeDOS"],
            "directory_metadata_partial_markers": partial_markers,
            "directory_fat_attributes_restored": True,
            "directory_timestamp_restoration": "reports partial because DOS handle timestamp API cannot open directories",
            "linux_reads_and_extracts_freedos_directory_archive": True,
            "low_space_replacement_returned_failure": True,
            "low_space_replacement_preserved_existing_archive_bytes": True,
            "low_space_replacement_left_no_stage_or_backup": True,
            "free_bytes_before_low_space_replacement": remaining_bytes,
            "explicit_backup_recovery_restored_missing_target": True,
            "recovery_refused_existing_target_without_modification": True,
            "recovery_refused_corrupt_backup_without_modification": True,
            "selected_backup_integrity_tested_before_restore": True,
            "process_exit_after_backup_rename_recovered_original": True,
            "paired_verified_stage_removed_after_recovery": True,
        },
    }
    (out / "run.json").write_text(json.dumps(run_record, indent=2) + "\n",
                                   encoding="utf-8")
    print("PASS: DOS16 CP850 lowercase ø.TXT transfer under FreeDOS; evidence:", out)


if __name__ == "__main__":
    main()
