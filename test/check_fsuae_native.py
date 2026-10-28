#!/usr/bin/env python3
"""Exercise Amiga/Linux exchange and metadata on a disposable native floppy."""
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
    p.add_argument("--amiga-cc", default="/opt/amiga/bin/m68k-amigaos-gcc")
    p.add_argument("--amiga-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--output", type=Path)
    p.add_argument("--timeout", type=int, default=90)
    p.add_argument("--filesystem", choices=("ofs", "ffs"), default="ofs")
    args = p.parse_args()
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="boltpack-amiga-" + args.filesystem + "-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "host"
    drive.mkdir()
    home = out / "home"
    (home / "Desktop").mkdir(parents=True)
    rom = args.rom.expanduser().resolve(strict=True)
    wb = args.workbench.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    system_disk, native = out / "workbench.adf", out / "data.adf"
    shutil.copyfile(wb, system_disk)
    shutil.copyfile(args.amiga_cli.resolve(strict=True), drive / "boltpack")
    payload = b"Amiga native filesystem interchange\n" * 100
    extra = b"append on native Amiga disk\n" * 70
    (drive / "SRC.TXT").write_bytes(payload)
    (drive / "NEW.TXT").write_bytes(extra)
    accent_name = "caf\u00e9.txt"
    (drive / accent_name).write_bytes(extra)
    emoji_name = "face\U0001f600.txt"
    (drive / emoji_name).write_bytes(extra)
    (drive / "Foo").mkdir()
    (drive / "foo").mkdir()
    (drive / "Foo" / "A.TXT").write_bytes(payload)
    (drive / "foo" / "B.TXT").write_bytes(extra)
    with (out / "commands.log").open("wb") as log:
        def run(command, cwd=None):
            log.write((repr(command) + "\n").encode()); log.flush()
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        run([args.amiga_cc, "-noixemul", "-O2", str(ROOT / "test/amiga/metadata_probe.c"),
             "-o", str(drive / "METAPROBE")])
        run([str(linux), "c", "--interchange-profile-1", "-1", "LINUX.BLT", "SRC.TXT"], drive)
        run([str(linux), "c", "--interchange-profile-1", "-1", "UNICODE.BLT", accent_name], drive)
        run([str(linux), "c", "--interchange-profile-1", "UNREP.BLT", "SRC.TXT", emoji_name], drive)
        run([str(linux), "c", "--interchange-profile-1", "CASE.BLT", "Foo/A.TXT", "foo/B.TXT"], drive)
        run([args.xdftool, str(native), "create"])
        fmt = [args.xdftool, str(native), "format", "BPNATIVE"]
        if args.filesystem == "ffs": fmt.append("ffs")
        run(fmt)
        for name in ("SRC.TXT", "NEW.TXT", "LINUX.BLT"):
            run([args.xdftool, str(native), "write", str(drive / name), name])
        run([args.xdftool, str(native), "write", str(drive / accent_name), accent_name])
        run([args.xdftool, str(native), "write", str(drive / "UNICODE.BLT"), "UNICODE.BLT"])
        for name in ("UNREP.BLT", "CASE.BLT"):
            run([args.xdftool, str(native), "write", str(drive / name), name])
        run([args.xdftool, str(native), "makedir", "OUT"])
        run([args.xdftool, str(native), "makedir", "RESTORE"])
        run([args.xdftool, str(native), "makedir", "UOUT"])
        run([args.xdftool, str(native), "makedir", "BADNAME"])
        run([args.xdftool, str(native), "makedir", "BADCASE"])
        startup = out / "startup"
        startup.write_text("\n".join([
            "C:SetPatch >NIL: QUIET", "C:Assign T: RAM:", "C:Stack 262144", "C:FailAt 100",
            "DH0:METAPROBE set DF1:SRC.TXT", "C:Echo $RC >DH0:SET.RC",
            "DH0:METAPROBE date DF1:SRC.TXT", "C:Echo $RC >DH0:DATE.RC",
            "DH0:METAPROBE time DF1:SRC.TXT >DH0:TIMEBEF.TXT",
            "DH0:METAPROBE get DF1:SRC.TXT >DH0:BEFORE.TXT",
            "DH0:boltpack x -o DF1:OUT DF1:LINUX.BLT >DH0:EXTRACT.LOG", "C:Echo $RC >DH0:EXTRACT.RC",
            "DH0:boltpack x -o DF1:UOUT DF1:UNICODE.BLT >DH0:UNICODE.LOG", "C:Echo $RC >DH0:UNICODE.RC",
            "DH0:boltpack c --interchange-profile-1 DF1:ACCENT.BLT DF1:caf\u00e9.txt >DH0:ACCENT.LOG", "C:Echo $RC >DH0:ACCENT.RC",
            "DH0:boltpack x -o DF1:BADNAME DF1:UNREP.BLT >DH0:UNREP.LOG", "C:Echo $RC >DH0:UNREP.RC",
            "DH0:boltpack x -o DF1:BADCASE DF1:CASE.BLT >DH0:CASE.LOG", "C:Echo $RC >DH0:CASE.RC",
            "DH0:boltpack c --interchange-profile-1 DF1:AMIGA.BLT DF1:SRC.TXT >DH0:CREATE.LOG", "C:Echo $RC >DH0:CREATE.RC",
            "DH0:boltpack a --interchange-profile-1 DF1:AMIGA.BLT DF1:NEW.TXT >DH0:APPEND.LOG", "C:Echo $RC >DH0:APPEND.RC",
            "C:Copy DF1:AMIGA.BLT DF1:SNAP.BLT QUIET",
            "DH0:boltpack a --interchange-profile-1 DF1:AMIGA.BLT DF1:SRC.TXT DF1:MISSING.TXT >DH0:FAILADD.LOG",
            "C:Echo $RC >DH0:FAILADD.RC",
            "C:Copy DF1:AMIGA.BLT DF1:FAILADD.BLT QUIET",
            "DH0:boltpack x -o DF1:RESTORE DF1:AMIGA.BLT >DH0:RESTORE.LOG", "C:Echo $RC >DH0:RESTORE.RC",
            "DH0:METAPROBE get DF1:RESTORE/SRC.TXT >DH0:AFTER.TXT",
            "DH0:METAPROBE time DF1:RESTORE/SRC.TXT >DH0:TIMEAFT.TXT",
            "DH0:boltpack d DF1:AMIGA.BLT NEW.TXT >DH0:DELETE.LOG", "C:Echo $RC >DH0:DELETE.RC",
            # Amiga floppy handlers defer writes; let them flush before stopping FS-UAE.
            "C:Wait 5", "C:Echo FINISHED >DH0:DONE.TXT", ""]), encoding="latin-1")
        run([args.xdftool, str(system_disk), "delete", "S/Startup-Sequence"])
        run([args.xdftool, str(system_disk), "write", str(startup), "S/Startup-Sequence"])
        env = os.environ.copy()
        env.update(HOME=str(home), LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
               "--amiga_model=A500+", "--kickstart_file=" + str(rom),
               "--floppy_drive_0=" + str(system_disk), "--floppy_drive_1=" + str(native),
               "--hard_drive_0=" + str(drive), "--floppy_drive_speed=800", "--warp_mode=1",
               "--fullscreen=0", "--chip_memory=2048", "--fast_memory=8192",
               "--writable_floppy_images=1"]
        log.write((repr(cmd) + "\n").encode()); log.flush()
        guest = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + args.timeout
            while not (drive / "DONE.TXT").is_file():
                if guest.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("FS-UAE stopped or timed out; evidence: " + str(out))
                time.sleep(0.25)
        finally:
            if guest.poll() is None: os.killpg(guest.pid, signal.SIGTERM)
            try: guest.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(guest.pid, signal.SIGKILL); guest.wait()
        for name in ("SET", "DATE", "EXTRACT", "UNICODE", "ACCENT", "CREATE", "APPEND", "RESTORE", "DELETE"):
            if (drive / (name + ".RC")).read_text().strip() != "0":
                raise RuntimeError("guest failed " + name + "; evidence: " + str(out))
        for name in ("FAILADD", "UNREP", "CASE"):
            if (drive / (name + ".RC")).read_text().strip() == "0":
                raise RuntimeError("native refusal returned success: " + name + "; evidence: " + str(out))
        before = (drive / "BEFORE.TXT").read_bytes()
        if (b"BoltPack archive comment" not in before or not (int(before.split(b":", 1)[0]) & 64) or
                before != (drive / "AFTER.TXT").read_bytes()):
            raise RuntimeError("native file metadata mismatch; evidence: " + str(out))
        if ((drive / "TIMEBEF.TXT").read_text().strip() != "10000:600:0" or
                (drive / "TIMEBEF.TXT").read_bytes() != (drive / "TIMEAFT.TXT").read_bytes()):
            raise RuntimeError("native file timestamp mismatch; evidence: " + str(out))
        for source, dest in (("OUT/SRC.TXT", "linux-to-amiga"),
                             ("RESTORE/SRC.TXT", "amiga-restored"),
                             ("RESTORE/NEW.TXT", "amiga-added"), ("AMIGA.BLT", "amiga.blt"),
                             ("SNAP.BLT", "before-failure.blt"), ("FAILADD.BLT", "after-failure.blt"),
                             ("UOUT/" + accent_name, "unicode-to-amiga"), ("ACCENT.BLT", "accent.blt")):
            run([args.xdftool, "-r", str(native), "read", source, str(out / dest)])
        for folder in ("BADNAME", "BADCASE"):
            listing = subprocess.run([args.xdftool, "-r", str(native), "list", folder],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
            (out / (folder + ".list")).write_bytes(listing)
            if len(listing.splitlines()) != 1:
                raise RuntimeError("native refusal modified output directory: " + folder)
    if ((out / "linux-to-amiga").read_bytes() != payload or
        (out / "amiga-restored").read_bytes() != payload or (out / "amiga-added").read_bytes() != extra or
        (out / "unicode-to-amiga").read_bytes() != extra):
        raise RuntimeError("native file bytes differ; evidence: " + str(out))
    if (out / "before-failure.blt").read_bytes() != (out / "after-failure.blt").read_bytes():
        raise RuntimeError("failed native append changed archive; evidence: " + str(out))
    accent_target = out / "linux-accent"
    accent_target.mkdir()
    accent_result = subprocess.run([str(linux), "x", "-o", str(accent_target), str(out / "accent.blt")],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-accent.log").write_bytes(accent_result.stdout)
    if (accent_result.returncode != 1 or b"some entries or attributes were not restored" not in accent_result.stdout or
            (accent_target / accent_name).read_bytes() != extra):
        raise RuntimeError("Amiga Latin-1 filename transfer failed; evidence: " + str(out))
    result = subprocess.run([str(linux), "l", str(out / "amiga.blt")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
    (out / "linux-list.log").write_bytes(result.stdout)
    if b"SRC.TXT" not in result.stdout or b"NEW.TXT" in result.stdout:
        raise RuntimeError("native delete result incorrect; evidence: " + str(out))
    target = out / "linux-output"
    target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(target), str(out / "amiga.blt")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if (result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout or
            (target / "SRC.TXT").read_bytes() != payload or (target / "NEW.TXT").exists()):
        raise RuntimeError("Linux extraction of native-created archive failed; evidence: " + str(out))
    (out / "manifest.txt").write_text("backend=Amiga " + args.filesystem.upper() + " floppy\nrom_sha256=" + hashlib.sha256(rom.read_bytes()).hexdigest() +
        "\nworkbench_sha256=" + hashlib.sha256(wb.read_bytes()).hexdigest() + "\n")
    print("PASS: native Amiga " + args.filesystem.upper() + " interchange, edits and file metadata; evidence:", out)


if __name__ == "__main__":
    main()
