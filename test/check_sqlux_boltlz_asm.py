#!/usr/bin/env python3
"""Check the 68000 BoltLZ assembly on the Sinclair QL (c68 + as68, sQLux).

Builds test/atari/boltlz_check.c with c68 twice, with the C codec and with
src/core/boltlz_m68k_c68.s, runs both in sQLux and requires identical
compressed streams (length and CRC-32), identical result codes for corrupted
streams, identical output for those that still decode, and no round-trip
failures. Output left in the buffer after a decoding error is not compared:
the QL's C decoder (BOLTLZ_FAST_DECODE=2) over-copies there by design.

With --compare FILE, the C build's streams must also equal another build's
BOLTLZ.TXT, for example from check_hatari_boltlz_asm.py --keep.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build(qcc, flags, sources, out):
    env = os.environ.copy()
    env["PATH"] = str(Path(shutil.which(qcc)).parent) + os.pathsep + env["PATH"]
    subprocess.run([qcc, "-O", "-I" + str(ROOT / "src/core")] + flags +
                   [str(ROOT / "test/atari/boltlz_check.c"),
                    str(ROOT / "src/core/boltlz.c")] + sources + ["-o", str(out)],
                   check=True, env=env, stdout=subprocess.DEVNULL)


def run(sqlux, rom, share, timeout):
    result = share / "boltlz_txt"
    env = os.environ.copy()
    env["TZ"] = "UTC"
    proc = subprocess.Popen(
        ["xvfb-run", "-a", str(sqlux), "--romdir", str(rom.parent), "--sysrom", rom.name,
         "--ramtop", "4096", "--device", "WIN1,%s,qdos-like" % share,
         "--boot_cmd", "EXEC_W win1_check"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
        start_new_session=True)
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if result.exists() and "done cases" in result.read_text(errors="replace"):
                break
            if proc.poll() is not None:
                break
            time.sleep(1)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        proc.wait()
    if not result.exists():
        raise RuntimeError("no result from sQLux in %s" % share)
    return result.read_text(errors="replace").replace("\r", "").splitlines()


def comparable(lines):
    """Streams and fuzz results, without buffer contents after an error."""
    out = []
    for line in lines:
        if line.startswith(("time ", "dtime ", "done ")):
            continue
        fields = line.split()
        if fields and fields[0] == "fuzz" and fields[3] != "0":
            line = " ".join(fields[:4])
        out.append(line)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--sqlux", type=Path, required=True)
    p.add_argument("--rom", type=Path, required=True)
    p.add_argument("--qcc", default="qcc")
    p.add_argument("--compare", type=Path, help="BOLTLZ.TXT from another build")
    p.add_argument("--timeout", type=int, default=1800)
    p.add_argument("--keep", action="store_true")
    args = p.parse_args()
    work = Path(tempfile.mkdtemp(prefix="boltlz-ql-asm-"))
    try:
        results = {}
        for name, flags, sources in (
                ("C", [], []),
                ("asm", ["-DBOLTLZ_M68K_ASM=1"],
                 [str(ROOT / "src/core/boltlz_m68k_c68.s")])):
            share = work / name
            share.mkdir()
            build(args.qcc, flags, sources, share / "check")
            lines = run(args.sqlux.resolve(), args.rom.resolve(), share, args.timeout)
            done = [l for l in lines if l.startswith("done ")]
            if not done or not done[0].endswith("failures 0") or \
                    any(l.startswith("FAIL") for l in lines):
                raise RuntimeError("%s build: %s" % (name, done or "incomplete"))
            results[name] = comparable(lines)
        if results["C"] != results["asm"]:
            diff = [(c, a) for c, a in zip(results["C"], results["asm"]) if c != a][:5]
            raise RuntimeError("assembly differs from C: %r" % diff)
        if args.compare:
            other = comparable(args.compare.read_text(errors="replace")
                               .replace("\r", "").splitlines())
            if other != results["C"]:
                raise RuntimeError("QL streams differ from %s" % args.compare)
        print("PASS QL: %d cases, assembly identical to C%s" %
              (len(results["C"]), " and to %s" % args.compare if args.compare else ""))
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
