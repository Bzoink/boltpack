#!/usr/bin/env python3
"""Volume sets (archive format 7) on the AmigaOS CLI under FS-UAE.

The guest creates a volume set from a source tree, lists, tests and reads
its info from numbered volumes, extracts it, extracts a set made by the
Linux CLI, and reports a missing volume.  The host then checks the restored
trees and extracts the Amiga-made set with the Linux CLI.  (--pause needs a
person at the console, so it is covered by test/check_volumes.py on the
hosts.)
"""
import argparse
import datetime
import filecmp
import os
from pathlib import Path
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def same_tree(a, b):
    """Compare two trees, ignoring the .uaem files FS-UAE keeps for the
    protection bits and comments of host-directory files."""
    cmp = filecmp.dircmp(a, b)
    cmp.left_only = [n for n in cmp.left_only if not n.endswith(".uaem")]
    cmp.right_only = [n for n in cmp.right_only if not n.endswith(".uaem")]
    cmp.common_files = [n for n in cmp.common_files if not n.endswith(".uaem")]
    cmp.diff_files = [n for n in cmp.diff_files if not n.endswith(".uaem")]
    if cmp.left_only or cmp.right_only or cmp.diff_files or cmp.funny_files:
        return False
    for f in cmp.common_files:
        if not filecmp.cmp(Path(a) / f, Path(b) / f, shallow=False):
            return False
    return all(same_tree(Path(a) / d, Path(b) / d) for d in cmp.common_dirs)


def run(cmd, cwd=None, log=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if log is not None:
        log.write(("$ %r\n" % (cmd,)).encode() + r.stdout)
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--rom", type=Path, required=True)
    p.add_argument("--workbench", type=Path, required=True)
    p.add_argument("--xdftool", default="xdftool")
    p.add_argument("--fs-uae", default="fs-uae")
    p.add_argument("--amiga-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--output", type=Path)
    p.add_argument("--timeout", type=int, default=900)
    args = p.parse_args()
    args.xdftool = shutil.which(args.xdftool) or args.xdftool
    out = args.output.resolve() if args.output else \
        Path(tempfile.mkdtemp(prefix="boltpack-amiga-vol-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "host"
    drive.mkdir()
    (out / "home" / "Desktop").mkdir(parents=True)
    rom = args.rom.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    system_disk = out / "workbench.adf"
    shutil.copyfile(args.workbench.expanduser().resolve(strict=True), system_disk)
    shutil.copyfile(args.amiga_cli.resolve(strict=True), drive / "boltpack")

    rnd = random.Random(11)
    src = drive / "SRC"
    (src / "SUB" / "DEEPER").mkdir(parents=True)
    (src / "TEXT.TXT").write_bytes(b"amiga volume set diskette archive " * 4000)
    (src / "SUB" / "RANDOM.BIN").write_bytes(
        bytes(rnd.getrandbits(8) for _ in range(150000)))
    (src / "SUB" / "DEEPER" / "MIXED.DAT").write_bytes(
        (b"mixed content block " * 200 +
         bytes(rnd.getrandbits(8) for _ in range(5000))) * 6)
    (src / "SUB" / "EMPTY").write_bytes(b"")
    for i in range(10):
        (src / "SUB" / ("SMALL%02d.TXT" % i)).write_bytes(b"small file %d\n" % i * (i + 1))

    with (out / "commands.log").open("wb") as log:
        # A set made by the Linux CLI for the guest to read, and one with a
        # volume left out.
        run([str(linux), "c", "-v", "64K", "-2", "LNX.BLT", "SRC"], drive, log)
        run([str(linux), "c", "-v", "64K", "-2", "MISS.BLT", "SRC"], drive, log)
        os.remove(drive / "MISS.001")
        tz_offset = int(datetime.datetime.now().astimezone()
                        .utcoffset().total_seconds() // 60)
        startup = out / "startup"
        startup.write_text("\n".join([
            "C:SetPatch >NIL: QUIET",
            "C:MakeDir RAM:ENV >NIL:",
            "C:Assign ENV: RAM:ENV",
            "C:SetEnv BP_TZ_OFFSET %d" % tz_offset,
            "C:Assign T: RAM:",
            "C:Stack 262144",
            "C:FailAt 100",
            "DH0:boltpack c -v 60K DH0:AMI.BLT DH0:SRC >DH0:CRT.LOG",
            "C:Echo $RC >DH0:CRT.RC",
            "DH0:boltpack l DH0:AMI.000 >DH0:LIST.LOG", "C:Echo $RC >DH0:LIST.RC",
            "DH0:boltpack t DH0:AMI.001 >DH0:TEST.LOG", "C:Echo $RC >DH0:TEST.RC",
            "DH0:boltpack i DH0:AMI.BLT >DH0:INFO.LOG", "C:Echo $RC >DH0:INFO.RC",
            "DH0:boltpack x -o DH0:OUT1 DH0:AMI.BLT >DH0:X1.LOG", "C:Echo $RC >DH0:X1.RC",
            "DH0:boltpack x -o DH0:OUT2 DH0:LNX.001 >DH0:X2.LOG", "C:Echo $RC >DH0:X2.RC",
            "DH0:boltpack t DH0:MISS.BLT <NIL: >DH0:MISS.LOG", "C:Echo $RC >DH0:MISS.RC",
            "DH0:boltpack c -v 60K DH0:AMI.001 DH0:SRC <NIL: >DH0:BADNAME.LOG",
            "C:Echo $RC >DH0:BADNAME.RC",
            "C:Wait 5", "C:Echo FINISHED >DH0:DONE.TXT", ""]), encoding="latin-1")
        run([args.xdftool, str(system_disk), "delete", "S/Startup-Sequence"], None, log)
        run([args.xdftool, str(system_disk), "write", str(startup),
             "S/Startup-Sequence"], None, log)
        env = os.environ.copy()
        env.update(HOME=str(out / "home"), LIBGL_ALWAYS_SOFTWARE="1",
                   SDL_AUDIODRIVER="dummy")
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
               "--amiga_model=A500+", "--kickstart_file=" + str(rom),
               "--floppy_drive_0=" + str(system_disk),
               "--hard_drive_0=" + str(drive), "--floppy_drive_speed=800",
               "--warp_mode=1", "--fullscreen=0", "--chip_memory=2048",
               "--fast_memory=8192", "--writable_floppy_images=1"]
        guest = subprocess.Popen(cmd, env=env, stdout=log,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + args.timeout
            while not (drive / "DONE.TXT").is_file():
                if guest.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("FS-UAE stopped or timed out; evidence: %s" % out)
                time.sleep(0.5)
        finally:
            if guest.poll() is None:
                os.killpg(guest.pid, signal.SIGTERM)
            try:
                guest.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(guest.pid, signal.SIGKILL)
                guest.wait()

    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("  FAIL  " + what)

    def rc(name):
        return int((drive / (name + ".RC")).read_text().strip())

    def text(name):
        return (drive / name).read_text(errors="replace")

    check(rc("CRT") == 0, "guest created a volume set (RC %s)" % rc("CRT"))
    vols = sorted(n for n in os.listdir(drive)
                  if n.startswith("AMI.") and not n.endswith((".TMP", ".uaem")))
    check(len(vols) >= 3 and vols[-1].upper() == "AMI.BLT",
          "several volumes, the last named AMI.BLT (%s)" % vols)
    check(not [n for n in os.listdir(drive) if n.startswith("bpvol")],
          "no staging files left behind")
    check(rc("LIST") == 0 and "SUB/RANDOM.BIN" in text("LIST.LOG"),
          "list from volume 1")
    check(rc("TEST") == 0, "test from volume 2")
    check(rc("INFO") == 0 and "Volumes:" in text("INFO.LOG") and
          "(all volumes)" in text("INFO.LOG"), "info shows the volumes")
    check(rc("X1") == 0 and same_tree(src, drive / "OUT1" / "SRC"),
          "guest extracted its own set")
    check(rc("X2") == 0 and same_tree(src, drive / "OUT2" / "SRC"),
          "guest extracted the Linux set")
    # AmigaDOS has no way to redirect stderr, so only the exit codes and the
    # standard output of these two failures can be checked here
    check(rc("MISS") != 0 and "FAILED" in text("MISS.LOG"),
          "a missing volume fails the test")
    check(rc("BADNAME") != 0 and not (drive / "AMI.001.NEW").exists(),
          "a numbered archive name is refused")
    # the Linux CLI reads the Amiga set
    # (a case-sensitive host needs the last volume's real name, AMI.BLT)
    r = run([str(linux), "t", str(drive / "AMI.BLT")])
    check(r.returncode == 0, "Linux tests the Amiga set")
    xdir = out / "linux-extract"
    r = run([str(linux), "x", "-o", str(xdir), str(drive / "AMI.BLT")])
    # exit 1 is "partial": the Amiga protection bits and comments are foreign
    # metadata on Linux
    check(r.returncode in (0, 1) and same_tree(src, xdir / "SRC"),
          "Linux extracts the Amiga set")
    if fails:
        print("%d check(s) failed; evidence: %s" % (len(fails), out))
        return 1
    print("Amiga volume checks passed")
    if not args.output:
        shutil.rmtree(out, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
