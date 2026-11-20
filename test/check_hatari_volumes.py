#!/usr/bin/env python3
"""Volume sets (archive format 7) on the Atari CLI under Hatari.

The guest creates a volume set from a source tree, lists, tests and reads
the info of numbered volumes, extracts its own set and a set made by the
Linux CLI, and fails cleanly on a missing volume and on a numbered archive
name.  The host checks the restored trees and extracts the Atari-made set
with the Linux CLI.  (--pause needs a person at the console, so it is
covered by test/check_volumes.py on the hosts.)
"""
import argparse
import filecmp
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def same_tree(a, b):
    """Compare two trees by lower-cased names: Hatari's GEMDOS drive reports
    8.3 names in lower case."""
    names_a = {n.lower(): n for n in os.listdir(a)}
    names_b = {n.lower(): n for n in os.listdir(b)}
    if set(names_a) != set(names_b):
        return False
    for low, na in names_a.items():
        pa, pb = Path(a) / na, Path(b) / names_b[low]
        if pa.is_dir() != pb.is_dir():
            return False
        if pa.is_dir():
            if not same_tree(pa, pb):
                return False
        elif not filecmp.cmp(pa, pb, shallow=False):
            return False
    return True


def run(cmd, cwd=None, log=None, env=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, env=env)
    if log is not None:
        log.write(("$ %r\n" % (cmd,)).encode() + r.stdout)
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tos", required=True, type=Path)
    p.add_argument("--hatari", default="hatari")
    p.add_argument("--cc", default="m68k-atari-mint-gcc")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--atari-cli", type=Path, default=ROOT / "build/atari/boltpack.tos")
    p.add_argument("--output", type=Path)
    p.add_argument("--run-vbls", type=int, default=40000)
    args = p.parse_args()
    out = args.output.resolve() if args.output else \
        Path(tempfile.mkdtemp(prefix="atari-volumes-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "drive"
    drive.mkdir()
    tos = args.tos.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    shutil.copyfile(args.atari_cli.resolve(strict=True), drive / "BOLTPACK.TOS")
    (drive / "NOINPUT.TXT").write_bytes(b"")

    rnd = random.Random(5)
    src = drive / "SRC"
    (src / "SUB" / "DEEPER").mkdir(parents=True)
    (src / "TEXT.TXT").write_bytes(b"atari volume set diskette archive " * 3000)
    (src / "SUB" / "RANDOM.BIN").write_bytes(
        bytes(rnd.getrandbits(8) for _ in range(90000)))
    (src / "SUB" / "DEEPER" / "MIXED.DAT").write_bytes(
        (b"mixed content block " * 150 +
         bytes(rnd.getrandbits(8) for _ in range(3000))) * 4)
    (src / "SUB" / "EMPTY").write_bytes(b"")
    for i in range(6):
        (src / "SUB" / ("SMALL%02d.TXT" % i)).write_bytes(b"small file %d\n" % i * (i + 1))

    with (out / "run.log").open("wb") as log:
        run([args.cc, "-O2", str(ROOT / "test/atari/guest_runner.c"), "-o",
             str(drive / "RUN.TOS")], ROOT, log)
        run([str(linux), "c", "-v", "24K", "-2", "LNX.BLT", "SRC"], drive, log)
        run([str(linux), "c", "-v", "24K", "-2", "MISS.BLT", "SRC"], drive, log)
        os.remove(drive / "MISS.001")
        commands = [
            r"c -v 24K C:\AMI.BLT C:\SRC",
            r"l C:\AMI.000",
            r"t C:\AMI.001",
            r"i C:\AMI.BLT",
            r"x -o C:\OUT1 C:\AMI.BLT",
            r"x -o C:\OUT2 C:\LNX.001",
            r"t C:\MISS.BLT",
            r"c -v 24K C:\AMI.001 C:\SRC",
        ]
        (drive / "COMMANDS.TXT").write_text("\n".join(commands) + "\n",
                                            encoding="ascii")
        for d in ("OUT1", "OUT2"):
            (drive / d).mkdir()
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        r = run([args.hatari, "--machine", "ste", "--tos", str(tos),
                 "--sound", "off", "--confirm-quit", "off",
                 "--fast-forward", "on", "--fast-boot", "on",
                 "--memsize", "4", "--harddrive", str(drive),
                 "--auto", r"C:\RUN.TOS", "--run-vbls", str(args.run_vbls)],
                ROOT, log, env=env)

    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("  FAIL  " + what)

    check((drive / "FINISH.TXT").is_file(), "the guest finished its commands")
    results = (drive / "RESULT.TXT").read_text().split() \
        if (drive / "RESULT.TXT").is_file() else []

    def result(i):
        return int(results[i]) if i < len(results) else None

    def text(i):
        f = drive / ("LOG%02d.TXT" % i)
        return f.read_text(errors="replace") if f.is_file() else ""

    check(result(0) == 0, "guest created a volume set (%s)" % result(0))
    vols = sorted(n for n in os.listdir(drive)
                  if n.startswith("AMI.") and not n.endswith(".TMP"))
    check(len(vols) >= 3 and vols[-1] == "AMI.BLT",
          "several volumes, the last named AMI.BLT (%s)" % vols)
    check(not [n for n in os.listdir(drive) if n.startswith("BP")
               and n != "BOLTPACK.TOS"], "no staging files left behind")
    check(result(1) == 0 and "sub/random.bin" in text(1).lower(),
          "list from volume 1")
    check(result(2) == 0, "test from volume 2")
    check(result(3) == 0 and "(all volumes)" in text(3), "info shows the volumes")
    check(result(4) in (0, 1) and same_tree(src, drive / "OUT1" / "SRC"),
          "guest extracted its own set")
    check(result(5) in (0, 1) and same_tree(src, drive / "OUT2" / "SRC"),
          "guest extracted the Linux set")
    check(result(6) not in (0, None) and "miss.001" in text(6).lower(),
          "a missing volume is named")
    check(result(7) not in (0, None) and "cannot name a volume set" in text(7),
          "a numbered archive name is refused")
    r = run([str(linux), "t", str(drive / "AMI.BLT")])
    check(r.returncode == 0, "Linux tests the Atari set")
    xdir = out / "linux-extract"
    r = run([str(linux), "x", "-o", str(xdir), str(drive / "AMI.BLT")])
    check(r.returncode in (0, 1) and same_tree(src, xdir / "SRC"),
          "Linux extracts the Atari set")
    if fails:
        print("%d check(s) failed; evidence: %s" % (len(fails), out))
        return 1
    print("Atari volume checks passed")
    if not args.output:
        shutil.rmtree(out, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
