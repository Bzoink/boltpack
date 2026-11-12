#!/usr/bin/env python3
"""Windows CLI under Wine: directory symlinks and junctions are not followed.

Before, a link that points back up the tree recursed until the path limit and the
whole create failed, and a link to a folder outside the tree pulled that folder's
files into the archive.  Wine flags a Unix directory symlink as a reparse point;
file symlinks are indistinguishable from files there, so they are archived as the
files they point to.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
passes = 0
failures = []


def check(ok, name, detail=""):
    global passes
    if ok:
        passes += 1
        print("  PASS  " + name)
    else:
        failures.append(name)
        print("  FAIL  " + name + ("  [" + detail + "]" if detail else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wine", default="wine")
    ap.add_argument("--exe", default=str(ROOT / "build/win64/boltpack.exe"))
    ap.add_argument("--linux-cli", default=str(ROOT / "build/linux/boltpack"))
    args = ap.parse_args()
    if shutil.which(args.wine) is None:
        print("SKIP: wine not installed")
        return 0
    work = Path(tempfile.mkdtemp(prefix="bp-winlinks-"))
    env = dict(os.environ, WINEDEBUG="-all")

    def wine(*a):
        return subprocess.run([args.wine, args.exe] + [str(x) for x in a], cwd=work,
                              env=env, text=True, capture_output=True, timeout=120)
    try:
        (work / "src/real").mkdir(parents=True)
        (work / "elsewhere").mkdir()
        (work / "src/real/f.txt").write_text("real\n")
        (work / "elsewhere/secret.txt").write_text("outside\n")
        os.symlink("../elsewhere", work / "src/linkdir")
        os.symlink(".", work / "src/loop")
        r = wine("c", "t.bp", "src")
        check(r.returncode == 0, "create succeeds although a link loops", r.stdout + r.stderr)
        check("src/loop is a symbolic link or junction; it is not archived" in r.stderr and
              "src/linkdir is a symbolic link or junction; it is not archived" in r.stderr,
              "each link is named in a warning", r.stderr)
        check("2 items were not archived" in r.stderr, "the summary counts them", r.stderr)
        listing = subprocess.run([args.linux_cli, "l", work / "t.bp"], text=True,
                                 capture_output=True).stdout
        check("src/real/f.txt" in listing and "secret.txt" not in listing and "loop" not in listing,
              "the real files are archived and nothing outside the tree is", listing)
        r = wine("t", "t.bp")
        check(r.returncode == 0, "the archive tests clean", r.stdout + r.stderr)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\nResults: %d passed, %d failed" % (passes, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
