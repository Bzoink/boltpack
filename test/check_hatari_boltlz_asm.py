#!/usr/bin/env python3
"""Check the hand-written 68000 BoltLZ encoder (src/core/boltlz_m68k.S).

Builds test/atari/boltlz_check.c twice, with the C encoder and with the
assembly, runs both in Hatari and requires byte-identical compressed output
(length and CRC-32 of every stream) and no round-trip failures. Prints the
compression times of both builds.

With --emutos, the same is repeated for -m68030 builds on an emulated TT,
which exercises the assembly's 68020+ variant (EmuTOS is needed there:
Hatari runs 68020+ CPUs only with EmuTOS or TOS 2.06+).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build(cc, flags, sources, out):
    cmd = [cc, "-ansi", "-Wall", "-Wno-unused-function", "-O2", "-I" + str(ROOT / "src/core")] + flags + \
        [str(ROOT / "test/atari/boltlz_check.c"), str(ROOT / "src/core/boltlz.c")] + \
        sources + ["-o", str(out)]
    subprocess.run(cmd, check=True)


def run(hatari, machine, tos, drive, timeout):
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    result = drive / "BOLTLZ.TXT"
    proc = subprocess.Popen(
        [hatari, "--machine", machine, "--tos", str(tos), "--sound", "off",
         "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
         "--memsize", "4", "--harddrive", str(drive), "--auto", r"C:\CHECK.TOS",
         "--run-vbls", "2000000"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if result.exists() and "done cases" in result.read_text(errors="replace"):
                break
            if proc.poll() is not None:
                break
            time.sleep(1)
    finally:
        proc.kill()
        proc.wait()
    if not result.exists():
        raise RuntimeError("no result from %s run in %s" % (machine, drive))
    return result.read_text(errors="replace").splitlines()


def split(lines):
    streams = [l for l in lines if not l.startswith(("time ", "done "))]
    times = [l for l in lines if l.startswith("time ")]
    done = [l for l in lines if l.startswith("done ")]
    return streams, times, done


def check_pair(args, label, machine, tos, cflags, work):
    outputs = {}
    for name, flags, sources in (
            ("C", cflags, []),
            ("asm", cflags + ["-DBOLTLZ_M68K_ASM=1"],
             [str(ROOT / "src/core/boltlz_m68k.S")])):
        drive = work / ("%s-%s" % (label, name))
        drive.mkdir()
        build(args.cc, flags, sources, drive / "CHECK.TOS")
        outputs[name] = split(run(args.hatari, machine, tos, drive, args.timeout))
    c_streams, c_times, c_done = outputs["C"]
    a_streams, a_times, a_done = outputs["asm"]
    for name, done in (("C", c_done), ("asm", a_done)):
        if not done or not done[0].endswith("failures 0"):
            raise RuntimeError("%s %s build: %s" % (label, name, done or "incomplete"))
        if any(l.startswith("FAIL") for l in outputs[name][0]):
            raise RuntimeError("%s %s build reported round-trip failures" % (label, name))
    if c_streams != a_streams:
        diff = [(c, a) for c, a in zip(c_streams, a_streams) if c != a][:5]
        raise RuntimeError("%s: assembly output differs from C: %r" % (label, diff))
    print("PASS %s: %s, assembly output identical to C" % (label, c_done[0]))
    for c, a in zip(c_times, a_times):
        level = c.split()[2]
        ct, at = int(c.split()[-1]), int(a.split()[-1])
        print("  level %s: C %d ticks, assembly %d ticks (%.2fx)" %
              (level, ct, at, ct / at if at else 0.0))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--tos", type=Path, required=True, help="TOS ROM for an ST/STE")
    p.add_argument("--emutos", type=Path, help="EmuTOS ROM: also test 68030 builds on a TT")
    p.add_argument("--machine", default="ste")
    p.add_argument("--hatari", default="hatari")
    p.add_argument("--cc", default="m68k-atari-mint-gcc")
    p.add_argument("--timeout", type=int, default=1800)
    p.add_argument("--keep", action="store_true", help="keep the work directory")
    args = p.parse_args()
    work = Path(tempfile.mkdtemp(prefix="boltlz-asm-"))
    try:
        check_pair(args, "68000", args.machine, args.tos, [], work)
        if args.emutos:
            check_pair(args, "68030", "tt", args.emutos, ["-m68030"], work)
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print("FAIL: %s (work directory %s)" % (exc, work))
        return 1
    if args.keep:
        print("work directory:", work)
    else:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
