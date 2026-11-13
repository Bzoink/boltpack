#!/usr/bin/env python3
"""Check that the macOS source kits are self-contained.

Builds every kit with scripts/create-macos-source-kits.sh into a scratch
directory, unpacks it, and verifies that (a) every quoted #include in the
kit's macOS sources resolves inside the kit and (b) the CLI frontend passes a
syntax-only compile against the Apple API stubs using only files from the kit.
A kit that is missing a header it needs would otherwise first fail on the
user's Mac.
"""

import os
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
failures = []
passes = 0


def check(ok, name, detail=""):
    global passes
    if ok:
        passes += 1
        print("  PASS  " + name)
    else:
        failures.append(name)
        print("  FAIL  " + name + ("  [" + detail + "]" if detail else ""))


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="boltpack-kits-") as scratch:
        dist = Path(scratch) / "dist"
        env = dict(os.environ, BOLTPACK_DIST_DIR=str(dist))
        r = subprocess.run(["bash", str(ROOT / "scripts/create-macos-source-kits.sh"),
                            "all"], env=env, capture_output=True, text=True)
        check(r.returncode == 0, "kit script runs", r.stderr)
        kits = sorted(dist.glob("*.tar.gz"))
        check(len(kits) == 4, "four kits are produced", str(kits))
        for kit in kits:
            name = kit.name
            dest = Path(scratch) / name.replace(".tar.gz", "")
            dest.mkdir()
            with tarfile.open(kit) as tar:
                tar.extractall(dest, filter="data") if hasattr(tarfile, "data_filter") else tar.extractall(dest)
            top = next(dest.iterdir())
            check((top / "VERSION").is_file() and
                  (top / "VERSION").read_text().strip() ==
                  (ROOT / "VERSION").read_text().strip(),
                  name + ": release version is included")
            missing = []
            for src in sorted((top / "src/frontend").glob("macosx*/*")):
                if src.suffix not in (".c", ".h", ".m", ".cpp"):
                    continue
                text = src.read_text(errors="replace")
                for inc in INCLUDE.findall(text):
                    # build-native.sh adds src/core and src/frontend/gui_common
                    # to the include path for the GUI sources.
                    if ((src.parent / inc).exists() or
                            (top / "src/core" / inc).exists() or
                            (top / "src/frontend/gui_common" / inc).exists()):
                        continue
                    missing.append("%s -> %s" % (src.relative_to(top), inc))
            check(not missing, name + ": every quoted include resolves in the kit",
                  "; ".join(missing[:4]))
            cli = top / "src/frontend/macosx/main.c"
            r = subprocess.run(
                [cc, "-ansi", "-pedantic", "-Wno-overlength-strings",
                 "-fsyntax-only", "-U__linux__", "-D__APPLE__", "-D_GNU_SOURCE",
                 "-I" + str(top / "src/core"),
                 "-I" + str(ROOT / "test/macosx_stubs"), str(cli)],
                capture_output=True, text=True)
            errors = [l for l in r.stderr.splitlines() if "error" in l]
            check(r.returncode == 0, name + ": CLI compiles from the kit alone",
                  "; ".join(errors[:3]))
            r = subprocess.run(
                [cc, "-ansi", "-pedantic", "-Wno-overlength-strings",
                 "-fsyntax-only", "-U__linux__", "-D__APPLE__", "-D_GNU_SOURCE",
                 "-DBP_FORCE_LEGACY_AT",
                 "-I" + str(top / "src/core"),
                 "-I" + str(ROOT / "test/macosx_stubs"), str(cli)],
                capture_output=True, text=True)
            errors = [l for l in r.stderr.splitlines() if "error" in l]
            check(r.returncode == 0,
                  name + ": CLI compiles with the pre-10.10 *at emulation",
                  "; ".join(errors[:3]))
    print("\nResults: %d passed, %d failed" % (passes, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
