#!/usr/bin/env python3
"""Check the Linux m68k BoltLZ assembly build against the C build under qemu-user.

The assembly must write archives of exactly the same size as the C build (the
two differ only in the archive header's creation time), extract each other's
archives, and exchange archives with the host build.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

CASES = (("-1",), ("-2",), ("-F", "-l", "3"), ("-F", "-l", "8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--asm", type=Path, required=True)
    parser.add_argument("--portable", type=Path, required=True)
    parser.add_argument("--boltpack", type=Path, required=True)
    args = parser.parse_args()
    qemu = shutil.which(args.qemu)
    if qemu is None:
        parser.error(f"qemu-user unavailable: {args.qemu}")
    asm, portable, host = (p.resolve() for p in
                           (args.asm, args.portable, args.boltpack))

    # Deterministic mixed data: text with repeats plus binary-ish noise.
    state = 12345
    noise = bytearray()
    for _ in range(40000):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        noise.append(state >> 16 & 0xFF)
    words = b"alpha bravo charlie delta echo foxtrot golf hotel india juliet "
    text = b"".join(words[(i * 7) % 40:(i * 7) % 40 + 23] + b"\n"
                    for i in range(30000))
    sources = {"text.dat": text, "bin.dat": bytes(noise) + text[:50000]}

    with tempfile.TemporaryDirectory(prefix="bp-m68k-asm-") as directory:
        work = Path(directory)
        for name, data in sources.items():
            (work / name).write_bytes(data)

        def run(*argv, **kw):
            process = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, timeout=600)
            if process.returncode:
                raise RuntimeError(process.stdout.decode(errors="replace"))

        def emulated(binary):
            return (qemu, str(binary))

        def same_files(directory_name):
            return all((work / directory_name / n).read_bytes() == d
                       for n, d in sources.items())

        for case in CASES:
            label = " ".join(case)
            sizes = {}
            for tag, binary in (("asm", asm), ("c", portable)):
                archive = f"{tag}.blt"
                (work / archive).unlink(missing_ok=True)
                run(*emulated(binary), "c", "-q", *case, "--single-thread",
                    archive, *sources)
                sizes[tag] = (work / archive).stat().st_size
            if sizes["asm"] != sizes["c"]:
                raise SystemExit(f"FAIL {label}: asm {sizes['asm']} bytes, "
                                 f"C {sizes['c']} bytes")
            for maker, extractor, name in (
                    ("asm", portable, "C extracts asm-made"),
                    ("c", asm, "asm extracts C-made"),
                    ("asm", host, "host extracts asm-made")):
                out = "out-" + (label + " " + name).replace(" ", "_")
                (work / out).mkdir()
                command = ((qemu, str(extractor)) if extractor != host
                           else (str(extractor),))
                run(*command, "x", "-q", "-o", out, f"{maker}.blt")
                if not same_files(out):
                    raise SystemExit(f"FAIL {label}: {name}")
    print("PASS: Linux m68k assembly build matches the C build and the host")


if __name__ == "__main__":
    main()
