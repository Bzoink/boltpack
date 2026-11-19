#!/usr/bin/env python3
"""Check the 68000 BoltLZ assembly on the Sharp X68000 (elf2x68k, run68).

Builds test/atari/boltlz_check.c with m68k-xelf-gcc twice, with the C codec
and with src/core/boltlz_m68k.S, runs both in the run68 Human68k emulator
and requires identical results (compressed streams, result codes and output
of corrupted streams) and no round-trip failures. Only 68000 builds are
checked: run68 does not execute 68020 instructions.

run68x 2.3.2 compares only 16 bits for CMPA.W, which makes correct GCC -O2
code fail; scripts/install-x68k-tools.sh builds it with the fix, and this
check refuses to run on an unpatched run68.

With --compare FILE, the streams must also equal another build's BOLTLZ.TXT,
for example from check_hatari_boltlz_asm.py --keep.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CMPA_CHECK = r'''
#include <stdio.h>
int main(void)
{
    unsigned long ne;
    __asm__ volatile("move.l #0x10000,%%a0\n\tmoveq #0,%0\n\tcmpa.w #0,%%a0\n\tsne %0"
                     : "=&d"(ne) : : "a0", "cc");
    return (ne & 0xFF) ? 0 : 1;
}
'''


def env_for(cc):
    env = os.environ.copy()
    path = shutil.which(cc)
    if path:
        env["PATH"] = str(Path(path).parent) + os.pathsep + env["PATH"]
    return env


def build(cc, flags, sources, out):
    subprocess.run([cc, "-ansi", "-O2", "-Wno-unused-function", "-I" + str(ROOT / "src/core")] +
                   flags + sources + ["-o", str(out)], check=True, env=env_for(cc))


def run68(run68_bin, workdir, program, timeout):
    # run68 needs short Human68k-style paths: run from the program's directory.
    return subprocess.run([run68_bin, "./" + program], cwd=workdir, timeout=timeout,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          stdin=subprocess.DEVNULL)


def comparable(text):
    out = []
    for line in text.replace("\r", "").splitlines():
        if not line.startswith(("time ", "dtime ", "done ")):
            out.append(line)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--cc", default="m68k-xelf-gcc")
    p.add_argument("--run68", default="run68")
    p.add_argument("--compare", type=Path, help="BOLTLZ.TXT from another build")
    p.add_argument("--timeout", type=int, default=1800)
    p.add_argument("--keep", action="store_true")
    args = p.parse_args()
    work = Path(tempfile.mkdtemp(prefix="boltlz-x68k-asm-"))
    try:
        probe = work / "probe"
        probe.mkdir()
        (probe / "cmpa.c").write_text(CMPA_CHECK)
        build(args.cc, [], [str(probe / "cmpa.c")], probe / "cmpa.x")
        if run68(args.run68, probe, "cmpa.x", 60).returncode != 0:
            raise RuntimeError("this run68 compares only 16 bits for CMPA.W; rebuild it with "
                               "scripts/install-x68k-tools.sh (scripts/patches/run68x-cmpa-w.patch)")
        results = {}
        for name, flags, sources in (
                ("C", [], []),
                ("asm", ["-DBOLTLZ_M68K_ASM=1"], [str(ROOT / "src/core/boltlz_m68k.S")])):
            d = work / name
            d.mkdir()
            build(args.cc, flags, [str(ROOT / "test/atari/boltlz_check.c"),
                                   str(ROOT / "src/core/boltlz.c")] + sources, d / "check.x")
            run68(args.run68, d, "check.x", args.timeout)
            result = d / "BOLTLZ.TXT"
            text = result.read_text(errors="replace") if result.exists() else ""
            done = [l for l in text.splitlines() if l.startswith("done ")]
            if not done or not done[0].rstrip().endswith("failures 0") or "FAIL" in text:
                raise RuntimeError("%s build: %s" % (name, done or "incomplete"))
            results[name] = comparable(text)
        if results["C"] != results["asm"]:
            diff = [(c, a) for c, a in zip(results["C"], results["asm"]) if c != a][:5]
            raise RuntimeError("assembly differs from C: %r" % diff)
        if args.compare and comparable(args.compare.read_text(errors="replace")) != results["C"]:
            raise RuntimeError("X68000 results differ from %s" % args.compare)
        print("PASS X68000: %d cases, assembly identical to C%s" %
              (len(results["C"]), " and to %s" % args.compare if args.compare else ""))
    except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print("FAIL: %s (work directory %s)" % (exc, work))
        return 1
    if args.keep:
        print("work directory:", work)
    else:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
