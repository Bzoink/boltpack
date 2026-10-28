#!/usr/bin/env python3
"""Run a basic Amiga/Linux Stored exchange on a disposable FS-UAE host mount."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--rom", type=Path, required=True)
    p.add_argument("--workbench", type=Path, required=True)
    p.add_argument("--xdftool", default="xdftool")
    p.add_argument("--fs-uae", default="fs-uae")
    p.add_argument("--amiga-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    p.add_argument("--amiga-cc", default="/opt/amiga/bin/m68k-amigaos-gcc")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--output", type=Path)
    p.add_argument("--stack", type=int, default=262144,
                   help="guest shell stack in bytes; 0 keeps its default")
    p.add_argument("--timeout", type=int, default=60)
    args = p.parse_args()
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="boltpack-fsuae-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "drive"
    drive.mkdir()
    (drive / "OUT").mkdir()
    (drive / "OUTLITE").mkdir()
    (drive / "TREEOUT").mkdir()
    (drive / "BADOUT").mkdir()
    (drive / "TREE" / "NEST" / "EMPTY").mkdir(parents=True)
    home = out / "home"
    (home / "Desktop").mkdir(parents=True)
    rom = args.rom.expanduser().resolve(strict=True)
    wb = args.workbench.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    disk = out / "workbench.adf"
    shutil.copyfile(wb, disk)
    shutil.copyfile(args.amiga_cli.resolve(strict=True), drive / "boltpack")
    data = b"Amiga and Linux exchange\n" * 128
    (drive / "SRC.TXT").write_bytes(data)
    sentinel = b"existing destination survives failed extraction\n"
    (drive / "BADOUT" / "SRC.TXT").write_bytes(sentinel)
    added = b"Amiga append data\n" * 128
    (drive / "NEW.TXT").write_bytes(added)
    (drive / "TREE" / "NEST" / "FILE.TXT").write_bytes(added)
    with (out / "commands.log").open("wb") as log:
        subprocess.run([args.amiga_cc, "-noixemul", "-O2", str(ROOT / "test/amiga/metadata_probe.c"),
                        "-o", str(drive / "METAPROBE")], stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(linux), "c", "--interchange-profile-1", "-0", "LINUX.BLT", "SRC.TXT"],
                       cwd=drive, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(linux), "c", "--interchange-profile-1", "-1", "LNX1.BLT", "SRC.TXT"],
                       cwd=drive, stdout=log, stderr=subprocess.STDOUT, check=True)
        corrupt = bytearray((drive / "LINUX.BLT").read_bytes())
        payload = corrupt.find(data)
        if payload < 0 or corrupt.find(data, payload + 1) >= 0:
            raise RuntimeError("stored archive payload not found uniquely")
        corrupt[payload + len(data) // 2] ^= 1
        (drive / "CORRUPT.BLT").write_bytes(corrupt)
        corruption_check = subprocess.run([str(linux), "t", str(drive / "CORRUPT.BLT")],
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log.write(corruption_check.stdout)
        if corruption_check.returncode == 0 or b"checksum mismatch" not in corruption_check.stdout:
            raise RuntimeError("corrupt fixture did not trigger checksum verification")
        startup = out / "startup"
        initial = ["C:SetPatch >NIL: QUIET", "C:Assign T: RAM:"]
        if args.stack:
            initial.append("C:Stack %d" % args.stack)
        startup.write_text("\n".join(initial + [
            "C:FailAt 100",
            "DH0:boltpack x -o DH0:OUT DH0:LINUX.BLT >DH0:EXTRACT.LOG",
            "C:Echo $RC >DH0:EXTRACT.RC",
            "DH0:boltpack x -o DH0:OUTLITE DH0:LNX1.BLT >DH0:EXLITE.LOG",
            "C:Echo $RC >DH0:EXLITE.RC",
            "DH0:boltpack c --interchange-profile-1 -0 DH0:AMIGA.BLT DH0:SRC.TXT >DH0:CREATE.LOG",
            "C:Echo $RC >DH0:CREATE.RC",
            "DH0:boltpack c --interchange-profile-1 DH0:LITE.BLT DH0:SRC.TXT >DH0:LITE.LOG",
            "C:Echo $RC >DH0:LITE.RC",
            "DH0:METAPROBE set DH0:LITE.BLT",
            "C:Echo $RC >DH0:META.RC",
            "DH0:METAPROBE get DH0:LITE.BLT >DH0:METABEF.TXT",
            "DH0:boltpack a --interchange-profile-1 DH0:LITE.BLT DH0:NEW.TXT >DH0:ADD.LOG",
            "C:Echo $RC >DH0:ADD.RC",
            "DH0:METAPROBE get DH0:LITE.BLT >DH0:METAAFT.TXT",
            "C:Copy DH0:LITE.BLT DH0:SNAP.BLT QUIET",
            "DH0:boltpack a --interchange-profile-1 DH0:LITE.BLT DH0:SRC.TXT DH0:MISSING.TXT >DH0:FAILADD.LOG",
            "C:Echo $RC >DH0:FAILADD.RC",
            "DH0:boltpack c --interchange-profile-1 DH0:LITE.BLT DH0:SRC.TXT DH0:MISSING.TXT >DH0:FAILCRT.LOG",
            "C:Echo $RC >DH0:FAILCRT.RC",
            "DH0:boltpack c --interchange-profile-1 DH0:NEWBAD.BLT DH0:SRC.TXT DH0:MISSING.TXT >DH0:FAILNEW.LOG",
            "C:Echo $RC >DH0:FAILNEW.RC",
            "C:Copy DH0:SNAP.BLT DH0:DELETE.BLT QUIET",
            "DH0:METAPROBE set DH0:DELETE.BLT",
            "C:Echo $RC >DH0:DELMETA.RC",
            "DH0:METAPROBE get DH0:DELETE.BLT >DH0:DELBEF.TXT",
            "DH0:boltpack d DH0:DELETE.BLT MISSING.TXT >DH0:FAILDEL.LOG",
            "C:Echo $RC >DH0:FAILDEL.RC",
            "C:Copy DH0:DELETE.BLT DH0:FAILDEL.BLT QUIET",
            "DH0:boltpack d DH0:DELETE.BLT NEW.TXT >DH0:DELETE.LOG",
            "C:Echo $RC >DH0:DELETE.RC",
            "DH0:METAPROBE get DH0:DELETE.BLT >DH0:DELAFT.TXT",
            "DH0:METAPROBE set DH0:TREE/NEST",
            "C:Echo $RC >DH0:DIRSET.RC",
            "DH0:METAPROBE set DH0:TREE/NEST/EMPTY",
            "C:Echo $RC >DH0:EMPSET.RC",
            "DH0:METAPROBE set DH0:TREE/NEST/FILE.TXT",
            "C:Echo $RC >DH0:FILESET.RC",
            "DH0:METAPROBE get DH0:TREE/NEST >DH0:DIRSRC.TXT",
            "DH0:METAPROBE get DH0:TREE/NEST/EMPTY >DH0:EMPSRC.TXT",
            "DH0:METAPROBE get DH0:TREE/NEST/FILE.TXT >DH0:FILESRC.TXT",
            "DH0:boltpack c --interchange-profile-1 DH0:TREE.BLT DH0:TREE >DH0:TREECRT.LOG",
            "C:Echo $RC >DH0:TREECRT.RC",
            "DH0:boltpack x -o DH0:TREEOUT DH0:TREE.BLT >DH0:TREEX.LOG",
            "C:Echo $RC >DH0:TREEX.RC",
            "DH0:METAPROBE get DH0:TREEOUT/TREE/NEST >DH0:DIRDST.TXT",
            "DH0:METAPROBE get DH0:TREEOUT/TREE/NEST/EMPTY >DH0:EMPDST.TXT",
            "DH0:METAPROBE get DH0:TREEOUT/TREE/NEST/FILE.TXT >DH0:FILEDST.TXT",
            "DH0:METAPROBE set DH0:BADOUT/SRC.TXT",
            "C:Echo $RC >DH0:BADSET.RC",
            "DH0:METAPROBE get DH0:BADOUT/SRC.TXT >DH0:BADBEF.TXT",
            "DH0:boltpack x -f -o DH0:BADOUT DH0:CORRUPT.BLT >DH0:BADX.LOG",
            "C:Echo $RC >DH0:BADX.RC",
            "DH0:METAPROBE get DH0:BADOUT/SRC.TXT >DH0:BADAFT.TXT",
            "DH0:boltpack c --interchange-profile-1 -F DH0:BAD.BLT DH0:SRC.TXT >DH0:BAD.LOG",
            "C:Echo $RC >DH0:BAD.RC", "C:Echo FINISHED >DH0:DONE.TXT", ""]), encoding="ascii")
        for command in ([args.xdftool, str(disk), "delete", "S/Startup-Sequence"],
                        [args.xdftool, str(disk), "write", str(startup), "S/Startup-Sequence"]):
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        env = os.environ.copy()
        env.update(HOME=str(home), LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
        command = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
                   "--amiga_model=A500+", "--kickstart_file=" + str(rom),
                   "--floppy_drive_0=" + str(disk), "--hard_drive_0=" + str(drive),
                   "--floppy_drive_speed=800", "--warp_mode=1", "--fullscreen=0",
                   "--chip_memory=2048", "--fast_memory=8192"]
        log.write((repr(command) + "\n").encode()); log.flush()
        guest = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            deadline = time.monotonic() + args.timeout
            while not (drive / "DONE.TXT").is_file():
                if guest.poll() is not None:
                    raise RuntimeError("FS-UAE exited before completion; see " + str(out))
                if time.monotonic() >= deadline:
                    raise RuntimeError("guest timed out; evidence: " + str(out))
                time.sleep(0.25)
        finally:
            if guest.poll() is None:
                os.killpg(guest.pid, signal.SIGTERM)
            try:
                guest.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(guest.pid, signal.SIGKILL)
                guest.wait()
    for name in ("CREATE.RC", "EXTRACT.RC", "EXLITE.RC", "LITE.RC", "META.RC", "ADD.RC", "DELMETA.RC", "DELETE.RC", "DIRSET.RC", "EMPSET.RC", "FILESET.RC", "TREECRT.RC", "TREEX.RC", "BADSET.RC"):
        if (drive / name).read_text().strip() != "0":
            raise RuntimeError("guest failure: " + name + "; evidence: " + str(out))
    if (drive / "OUT" / "SRC.TXT").read_bytes() != data:
        raise RuntimeError("Amiga extraction mismatch; evidence: " + str(out))
    if (drive / "OUTLITE" / "SRC.TXT").read_bytes() != data:
        raise RuntimeError("Amiga BoltLite extraction mismatch; evidence: " + str(out))
    if (drive / "BAD.RC").read_text().strip() == "0" or (drive / "BAD.BLT").exists():
        raise RuntimeError("incompatible profile options were not refused; evidence: " + str(out))
    if ((drive / "BADX.RC").read_text().strip() == "0" or
            (drive / "BADOUT" / "SRC.TXT").read_bytes() != sentinel or
            (drive / "BADBEF.TXT").read_bytes() != (drive / "BADAFT.TXT").read_bytes() or
            any(p.name not in ("SRC.TXT", "SRC.TXT.uaem") for p in (drive / "BADOUT").iterdir())):
        raise RuntimeError("corrupt extraction changed existing file, metadata, or left stage; evidence: " + str(out))
    for name in ("FAILADD.RC", "FAILCRT.RC", "FAILNEW.RC", "FAILDEL.RC"):
        if (drive / name).read_text().strip() == "0":
            raise RuntimeError("failed operation returned success: " + name)
    if (drive / "LITE.BLT").read_bytes() != (drive / "SNAP.BLT").read_bytes():
        raise RuntimeError("failed create/append modified existing archive; evidence: " + str(out))
    if (drive / "FAILDEL.BLT").read_bytes() != (drive / "SNAP.BLT").read_bytes():
        raise RuntimeError("failed delete modified existing archive; evidence: " + str(out))
    before = (drive / "METABEF.TXT").read_bytes()
    delete_before = (drive / "DELBEF.TXT").read_bytes()
    if (b"BoltPack archive comment" not in before or not (int(before.split(b":", 1)[0]) & 64) or
            b"BoltPack archive comment" not in delete_before or
            not (int(delete_before.split(b":", 1)[0]) & 64) or
            before != (drive / "METAAFT.TXT").read_bytes() or
            delete_before != (drive / "DELAFT.TXT").read_bytes()):
        raise RuntimeError("archive native protection/comment changed during replacement; evidence: " + str(out))
    if (drive / "NEWBAD.BLT").exists() or list(drive.glob("BP??????")):
        raise RuntimeError("failed create/append left output or stage; evidence: " + str(out))
    for prefix in ("DIR", "EMP", "FILE"):
        original_meta = (drive / (prefix + "SRC.TXT")).read_bytes()
        restored_meta = (drive / (prefix + "DST.TXT")).read_bytes()
        if (b"BoltPack archive comment" not in original_meta or
                not (int(original_meta.split(b":", 1)[0]) & 64) or original_meta != restored_meta):
            raise RuntimeError("archived %s metadata mismatch; evidence: %s" % (prefix, out))
    restored = drive / "TREEOUT" / "TREE" / "NEST"
    if not (restored / "EMPTY").is_dir() or (restored / "FILE.TXT").read_bytes() != added:
        raise RuntimeError("archived tree content mismatch; evidence: " + str(out))
    target = out / "linux-output"
    target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(target), str(drive / "AMIGA.BLT")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout:
        raise RuntimeError("unexpected Linux metadata result; evidence: " + str(out))
    if (target / "SRC.TXT").read_bytes() != data:
        raise RuntimeError("Linux extraction mismatch; evidence: " + str(out))
    lite_target = out / "linux-lite"
    lite_target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(lite_target), str(drive / "LITE.BLT")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-lite.log").write_bytes(result.stdout)
    if result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout:
        raise RuntimeError("unexpected Linux BoltLite metadata result; evidence: " + str(out))
    if (lite_target / "SRC.TXT").read_bytes() != data or (lite_target / "NEW.TXT").read_bytes() != added:
        raise RuntimeError("Linux BoltLite extraction/append mismatch; evidence: " + str(out))
    deleted_target = out / "linux-delete"
    deleted_target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(deleted_target), str(drive / "DELETE.BLT")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-delete.log").write_bytes(result.stdout)
    if (result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout or
            (deleted_target / "SRC.TXT").read_bytes() != data or
            (deleted_target / "NEW.TXT").exists()):
        raise RuntimeError("delete result was incorrect; evidence: " + str(out))
    info = subprocess.run([str(linux), "i", str(drive / "LITE.BLT")],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
    if b"BoltLite" not in info or (drive / "LITE.BLT").stat().st_size >= len(data) + len(added):
        raise RuntimeError("profile did not exercise BoltLite; evidence: " + str(out))
    (out / "manifest.txt").write_text("backend=FS-UAE host directory\nrom_sha256=" +
        hashlib.sha256(rom.read_bytes()).hexdigest() + "\nworkbench_sha256=" +
        hashlib.sha256(wb.read_bytes()).hexdigest() + "\npayload_sha256=" +
        hashlib.sha256(data).hexdigest() + "\n")
    print("PASS: Amiga/Linux profile exchange and staged edits; evidence:", out)


if __name__ == "__main__":
    main()
