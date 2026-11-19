#!/usr/bin/env python3
"""Compare native CP/M-86 BoltLite builds and exchange archives with Linux."""
import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emu2", required=True)
    parser.add_argument("--portable", type=Path, required=True)
    parser.add_argument("--optimized", type=Path, required=True)
    parser.add_argument("--boltpack", type=Path, required=True)
    args = parser.parse_args()
    emu2 = shutil.which(args.emu2)
    if emu2 is None:
        parser.error(f"CP/M-86 emulator unavailable: {args.emu2}")
    emu2 = Path(emu2).resolve()
    boltpack = args.boltpack.resolve()

    with tempfile.TemporaryDirectory(prefix="bp-cpm86-boltlite-") as directory:
        work = Path(directory)
        data = (b"BoltLite CP/M-86 interoperability. " * 750 +
                bytes(i % 217 for i in range(19477)))
        (work / "DATA.BIN").write_bytes(data)

        def run(*argv):
            process = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, timeout=30)
            if process.returncode:
                raise RuntimeError(process.stdout.decode(errors="replace"))
            return process.stdout

        def guest(*argv):
            command = shlex.join((str(emu2), "-c", "8086", *argv))
            return run("script", "-qec", command, "/dev/null")

        for name, exe in (("ref", args.portable), ("asm", args.optimized)):
            shutil.copyfile(exe, work / f"{name.upper()}.CMD")
            log = guest(f"{name.upper()}.CMD", "C", f"{name.upper()}.BLT", "DATA.BIN")
            if not (work / f"{name}.blt").is_file():
                raise AssertionError(log.decode(errors="replace"))
            log = guest(f"{name.upper()}.CMD", "T", f"{name.upper()}.BLT")
            if b"DATA.BIN  OK" not in log:
                raise AssertionError(log.decode(errors="replace"))

        if (work / "ref.blt").read_bytes() != (work / "asm.blt").read_bytes():
            raise AssertionError("CP/M-86 C and assembly builds emitted different archives")
        run(boltpack, "t", "asm.blt")
        run(boltpack, "c", "--target-profile=cpm86-83", "-1", "-b", "16k",
            "linux.blt", "DATA.BIN")
        (work / "DATA.BIN").unlink()

        for name in ("REF.BLT", "LINUX.BLT"):
            log = guest("ASM.CMD", "X", name)
            output = work / "data.bin"
            if not output.exists() or output.read_bytes() != data:
                raise AssertionError(f"{name} extraction failed: {log.decode(errors='replace')}")
            output.unlink()

    print("PASS: CP/M-86 C/8086 archive parity and Linux interoperability")


if __name__ == "__main__":
    main()
