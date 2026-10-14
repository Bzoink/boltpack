#!/usr/bin/env python3
"""Run native BoltLZ tests and distinguish cross compilation from runtime tests.

All builds use -O2. Cross probes use Clang's freestanding headers and minimal
string declarations, not a target libc; they make no runtime/speed claim.
Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLAGS = ["-ansi", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-function", "-Werror", "-O2"]
PORTABLE = [f"-DBOLTLZ_{name}=0" for name in (
    "WORD_MATCH", "SHORT_COPY", "SMALL_DISTANCE", "FAST_DECODE", "CHEAP_COST",
    "SPECIALIZE", "INLINE_FIND", "ENCODE_SHORT_COPY", "DECODE_DISTANCE3",
    "NARROW_TABLE", "FULL_CAPACITY")]
TARGETS = [
    ("aarch64", "aarch64-unknown-none-elf", []),
    ("aarch64-be", "aarch64_be-unknown-none-elf", []),
    ("armv7", "armv7-none-eabi", []),
    ("armv7-be", "armebv7-none-eabi", []),
    ("ppc32", "powerpc-unknown-none-elf", []),
    ("m68k", "m68k-unknown-none-elf", []),
    ("i686", "i686-unknown-none-elf", []),
    ("msp430", "msp430-unknown-none-elf", ["-DSIZEOF_INT=2"]),
]
SOURCES = ["src/core/boltlz.c", "src/core/boltlz.h", "src/core/boltlz_config.h",
           "src/core/bp_types.h", "test/unit/test_boltlz.c", "test/unit/test_runner.h",
           "test/reference/boltlz_scalar_decoder.h", "test/reference/boltlz_loop_base.c",
           "test/reference/boltlz_loop_base.h", "test/reference/boltlz_loop_base_config.h",
           "test/validate_boltlz.py"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/boltlz/platform-validation")
    parser.add_argument("--native-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rows = []

    def run(name: str, commands: list[list[str]], kind: str, **metadata) -> None:
        output = []
        row = {"name": name, "kind": kind, "commands": commands, **metadata}
        for command in commands:
            process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            output.append("$ " + " ".join(command) + "\n" + process.stdout + process.stderr)
            if process.returncode:
                row.update(status="failed", exit_code=process.returncode)
                break
        else:
            row["status"] = "passed"
        (args.output / (name + ".log")).write_text("\n".join(output))
        rows.append(row)
        print(f"{name}: {row['status']} ({kind})", flush=True)

    for compiler in ("clang", "gcc"):
        executable = shutil.which(compiler)
        if not executable:
            rows.append({"name": compiler, "kind": "native-runtime", "status": "unavailable"})
            continue
        version = subprocess.check_output([executable, "--version"], text=True).splitlines()[0]
        for mode, controls in (("default", []), ("portable", PORTABLE)):
            name = f"{compiler}-{mode}"
            binary = str((args.output / name).resolve())
            command = [executable, *FLAGS, *controls, "-Isrc/core", "test/unit/test_boltlz.c",
                       "src/core/boltlz.c", "test/reference/boltlz_loop_base.c", "-o", binary]
            run(name, [command, [binary]], "native-runtime", compiler=version)

    clang = shutil.which("clang")
    if clang and not args.native_only:
        resource = subprocess.check_output([clang, "-print-resource-dir"], text=True).strip()
        include = args.output / "probe-include"
        include.mkdir(exist_ok=True)
        (include / "string.h").write_text(
            "#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n"
            "void *memset(void *, int, size_t);\n")
        for name, target, target_flags in TARGETS:
            for mode, controls in (("default", []), ("portable", PORTABLE)):
                name_mode = f"{name}-{mode}"
                obj = str((args.output / (name_mode + ".o")).resolve())
                command = [clang, "-target", target, *FLAGS, *controls, *target_flags,
                           "-ffreestanding", "-nostdinc", "-isystem", resource + "/include",
                           "-I", str(include.resolve()), "-Isrc/core", "-c", "src/core/boltlz.c",
                           "-o", obj]
                run(name_mode, [command], "cross-compile-only", target=target)
                if rows[-1]["status"] == "passed":
                    header = Path(obj).read_bytes()[:20]
                    if header[:4] != b"\x7fELF":
                        rows[-1]["status"] = "failed"
                        rows[-1]["reason"] = "Expected a target ELF object"
                    else:
                        rows[-1].update(elf_bits={1: 32, 2: 64}[header[4]],
                                        byte_order={1: "little", 2: "big"}[header[5]])
    elif not args.native_only:
        rows.append({"name": "clang-cross-probes", "kind": "cross-compile-only", "status": "unavailable"})

    report = {"schema": 1, "host": platform.platform(), "flags": FLAGS, "portable_controls": PORTABLE,
              "limits": "Cross compilation does not test linking, target libc, runtime correctness, or speed.",
              "source_hashes": {s: hashlib.sha256((ROOT / s).read_bytes()).hexdigest() for s in SOURCES},
              "checks": rows}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    return int(any(row["status"] == "failed" for row in rows) or
               not any(row["kind"] == "native-runtime" and row["status"] == "passed" for row in rows))


if __name__ == "__main__":
    raise SystemExit(main())
