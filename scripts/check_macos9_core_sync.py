#!/usr/bin/env python3
"""Verify that the Classic Mac 68k kit's copied core matches src/core."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
SHARED = ROOT / "src/core"
COPIED = ROOT / "src/frontend/macos9/68k/Source"
CORE_FILES = (
    "archive_name.h",
    "archiver.c",
    "archiver.h",
    "boltlite.c",
    "boltlite.h",
    "boltlz.c",
    "boltlz.h",
    "boltlz_config.h",
    "bp_codepage.c",
    "bp_codepage.h",
    "bp_sjis_table.h",
    "bp_metadata.h",
    "bp_types.h",
    "compress.c",
    "compress.h",
    "fluff.c",
    "fluff.h",
)
SHARED_CORE_FILES = {
    "archive_name.h",
    "archiver.c",
    "archiver.h",
    "boltlite.c",
    "boltlite.h",
    "boltlz.c",
    "boltlz.h",
    "boltlz_config.h",
    "bp_codepage.c",
    "bp_codepage.h",
    "bp_sjis_table.h",
    "bp_metadata.h",
    "bp_types.h",
    "compress.c",
    "compress.h",
    "fluff.c",
    "fluff.h",
}
INTENTIONALLY_NOT_COPIED = {
    "boltpack_dll.c",
    "boltpack_dll.h",
    "bp_time.h",
    "fluff_fast.c",
    "fluff_fast.h",
    "fluff_json.c",
    "fluff_json.h",
    "fluff_simd.h",
    "fluff_trace.c",
    "fluff_trace.h",
    "zstd_wrap.c",
    "zstd_wrap.h",
}


def main() -> int:
    mismatches = []
    actual_shared = {path.name for path in SHARED.iterdir()
                     if path.suffix in (".c", ".h")}
    accounted = SHARED_CORE_FILES | INTENTIONALLY_NOT_COPIED
    if actual_shared != accounted:
        missing = sorted(actual_shared - accounted)
        stale = sorted(accounted - actual_shared)
        if missing:
            mismatches.append("new or uncategorized src/core files: " + ", ".join(missing))
        if stale:
            mismatches.append("accounted src/core files no longer present: " + ", ".join(stale))
    copied_shared = {path.name for path in COPIED.iterdir()
                     if path.suffix in (".c", ".h") and path.name != "main.c"}
    if copied_shared != SHARED_CORE_FILES:
        absent = sorted(SHARED_CORE_FILES - copied_shared)
        extra = sorted(copied_shared - SHARED_CORE_FILES)
        if absent:
            mismatches.append("missing copied core files: " + ", ".join(absent))
        if extra:
            mismatches.append("uncategorized files in copied core directory: " +
                              ", ".join(extra))
    for name in CORE_FILES:
        shared = SHARED / name
        copied = COPIED / name
        if not shared.is_file() or not copied.is_file():
            mismatches.append(f"{name}: missing shared or copied file")
        elif shared.read_bytes() != copied.read_bytes():
            mismatches.append(f"{name}: copied source differs from src/core")
    if mismatches:
        print("Classic Mac 68k copied-core check failed:", file=sys.stderr)
        for mismatch in mismatches:
            print(f"  {mismatch}", file=sys.stderr)
        return 1
    print(f"Classic Mac 68k copied core matches src/core ({len(CORE_FILES)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
