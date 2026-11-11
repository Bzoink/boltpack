#!/usr/bin/env python3
"""Check the pathname-based POSIX wrappers: build, archive edits, private-root extraction.

QNX, HP-UX, IRIX, NeXTSTEP, SCO, Tru64 and pre-7.3 AIX have no openat() family,
so they extract by pathname.  Pathname extraction cannot stop another process from
swapping a directory between a check and its use, so these wrappers extract only
into a directory they create themselves, private to the current user (mode 0700),
and open it to its normal permissions after extraction finishes.

This test compiles each wrapper on a Linux host with __linux__ undefined, which
selects the same pathname branch those systems build, and runs it against a
normal Linux-created archive.  It exercises BoltPack's logic; it does not touch
any of those operating systems.
"""

import argparse
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WRAPPERS = ["qnx", "hpux", "irix", "nextstep", "sco", "tru64", "aix"]
CORE = ["archiver", "compress", "boltlite", "fluff", "bp_codepage", "boltlz"]

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


def run(cmd, cwd, umask=None):
    def pre():
        if umask is not None:
            os.umask(umask)
    return subprocess.run([str(c) for c in cmd], cwd=cwd, text=True,
                          capture_output=True, preexec_fn=pre)


def mode_of(path):
    return stat.S_IMODE(os.lstat(path).st_mode)


def build(cc, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    objs = []
    for name in CORE:
        obj = outdir / (name + ".o")
        subprocess.run([cc, "-O2", "-Isrc/core", "-c", "-o", obj,
                        ROOT / "src/core" / (name + ".c")], cwd=ROOT, check=True)
        objs.append(str(obj))
    bins = {}
    for wrapper in WRAPPERS:
        obj = outdir / (wrapper + ".o")
        r = subprocess.run([cc, "-ansi", "-pedantic", "-Wall", "-Wextra",
                            "-Wno-unused-function", "-O2", "-U__linux__",
                            "-DBP_VERSION_MAJOR=1", "-DBP_VERSION_MINOR=10",
                            "-Isrc/core", "-c", "-o", obj,
                            "src/frontend/%s/main.c" % wrapper], cwd=ROOT,
                           capture_output=True, text=True)
        check(r.returncode == 0 and r.stderr == "",
              "%s: builds in pathname mode without warnings" % wrapper, r.stderr[:300])
        if wrapper == "nextstep":
            # NeXTSTEP has no <sys/statvfs.h>: building with that header
            # replaced by an #error proves the wrapper does not need it.
            fake = outdir / "nostatvfs/sys"
            fake.mkdir(parents=True, exist_ok=True)
            (fake / "statvfs.h").write_text('#error "sys/statvfs.h must not be included"\n')
            r = subprocess.run([cc, "-ansi", "-pedantic", "-Wall", "-Wextra",
                                "-Wno-unused-function", "-O2", "-U__linux__",
                                "-I" + str(outdir / "nostatvfs"),
                                "-DBP_VERSION_MAJOR=1", "-DBP_VERSION_MINOR=10",
                                "-Isrc/core", "-fsyntax-only",
                                "src/frontend/nextstep/main.c"], cwd=ROOT,
                               capture_output=True, text=True)
            check(r.returncode == 0,
                  "nextstep: builds without <sys/statvfs.h>", r.stderr[:300])
        exe = outdir / ("boltpack_" + wrapper)
        subprocess.run([cc, "-o", exe, obj] + objs, check=True)
        bins[wrapper] = exe
    return bins


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--linux-boltpack", default=str(ROOT / "build/linux/boltpack"))
    args = ap.parse_args()
    linux = Path(args.linux_boltpack).resolve()

    work = Path(tempfile.mkdtemp(prefix="bp_path_extract_"))
    try:
        bins = build(args.cc, work / "bin")
        for wrapper, exe in bins.items():
            print("\n--- %s ---" % wrapper)
            t = work / wrapper
            t.mkdir()
            src = t / "src"
            (src / "dir/sub").mkdir(parents=True)
            (src / "empty").mkdir()
            (src / "a.txt").write_bytes(b"alpha\n")
            (src / "dir/b.bin").write_bytes(bytes(range(256)) * 40)
            (src / "dir/sub/c.sh").write_bytes(b"#!/bin/sh\n")
            os.chmod(src / "dir/sub/c.sh", 0o755)
            old = 981173106
            for p in (src / "a.txt", src / "dir/b.bin", src / "dir/sub/c.sh",
                      src / "dir/sub", src / "dir"):
                os.utime(p, (old, old))

            # The wrapper itself creates, tests, edits and lists archives.
            r = run([exe, "c", "w.bp", "src"], t)
            check(r.returncode == 0, "create", r.stderr)
            r = run([exe, "t", "w.bp"], t)
            check(r.returncode == 0, "test", r.stderr)
            r = run([exe, "l", "w.bp"], t)
            check("src/dir/b.bin" in r.stdout and "src/empty/" in r.stdout,
                  "list shows files and the empty directory", r.stdout)
            (t / "more.txt").write_text("more")
            r = run([exe, "a", "w.bp", "more.txt"], t)
            check(r.returncode == 0 and "more.txt" in run([exe, "l", "w.bp"], t).stdout,
                  "append", r.stderr)
            r = run([exe, "d", "w.bp", "more.txt"], t)
            check(r.returncode == 0 and "more.txt" not in run([exe, "l", "w.bp"], t).stdout,
                  "delete", r.stderr)

            # Refusals leave nothing behind.
            r = run([exe, "x", "-n", "-o", "dry", "w.bp"], t)
            check(r.returncode == 0 and not (t / "dry").exists(),
                  "dry run succeeds and creates nothing", r.stderr)
            r = run([exe, "x", "w.bp"], t)
            check(r.returncode != 0 and "needs -o DIR" in r.stderr and
                  not (t / "a.txt").exists() and not (t / "dir").exists(),
                  "extraction without -o is refused and writes nothing", r.stderr)
            (t / "exists").mkdir()
            (t / "exists/keep.txt").write_text("keep")
            r = run([exe, "x", "-o", "exists", "w.bp"], t)
            check(r.returncode != 0 and "already exists" in r.stderr and
                  [p.name for p in (t / "exists").iterdir()] == ["keep.txt"],
                  "an existing output directory is refused and left alone", r.stderr)
            r = run([exe, "x", "-o", "missing/deeper", "w.bp"], t)
            check(r.returncode != 0 and not (t / "missing").exists(),
                  "a missing parent is refused and not created", r.stderr)

            # A fresh private directory extracts normally.
            r = run([exe, "x", "-o", "out", "w.bp"], t, umask=0o022)
            check(r.returncode == 0, "extraction into a new directory succeeds", r.stderr)
            out = t / "out/src"
            check((out / "a.txt").read_bytes() == b"alpha\n" and
                  (out / "dir/b.bin").read_bytes() == bytes(range(256)) * 40,
                  "file contents are exact")
            check((out / "empty").is_dir(), "the empty directory is created")
            check(mode_of(out / "dir/sub/c.sh") == 0o755, "permission bits are restored")
            check(int((out / "a.txt").stat().st_mtime) == old and
                  int((out / "dir/sub").stat().st_mtime) == old,
                  "file and directory times are restored")
            check(mode_of(t / "out") == 0o755,
                  "the output directory gets its normal permissions afterwards",
                  oct(mode_of(t / "out")))
            r = run([exe, "x", "-o", "out77", "w.bp"], t, umask=0o077)
            check(mode_of(t / "out77") == 0o700,
                  "the final permissions follow the umask", oct(mode_of(t / "out77")))

            # A relative symlink is recreated; its own time cannot be set here,
            # which is reported rather than hidden.
            os.symlink("a.txt", src / "lnk")
            run([exe, "c", "l.bp", "src"], t)
            r = run([exe, "x", "-o", "lout", "l.bp"], t)
            check(os.readlink(t / "lout/src/lnk") == "a.txt" and
                  (r.returncode == 0 or "symlink time not restored" in r.stderr),
                  "a relative symlink is recreated; a missing link time is reported",
                  r.stderr)
            os.symlink("/etc/passwd", src / "abs")
            run([exe, "c", "u.bp", "src"], t)
            r = run([exe, "x", "-o", "uout", "u.bp"], t)
            check(not (t / "uout/src/abs").exists() and r.returncode != 0,
                  "an absolute symlink target is refused", r.stderr)
            os.unlink(src / "abs")

            # An archive that fails partway leaves the directory private.
            r = run([linux, "c", "-0", "s.bp", "src"], t)
            data = bytearray((t / "s.bp").read_bytes())
            data[len(data) // 3] ^= 0xFF
            (t / "bad.bp").write_bytes(bytes(data))
            r = run([exe, "x", "-o", "bad", "bad.bp"], t, umask=0o022)
            check(r.returncode != 0 and mode_of(t / "bad") == 0o700 and
                  "left private" in r.stderr,
                  "a failed extraction leaves the directory private (0700)",
                  r.stderr + oct(mode_of(t / "bad")))
            check(not list((t / "bad").rglob(".boltpack-*")),
                  "no staging files are left behind")

        # Control: the Linux CLI is unchanged (descriptor-relative extraction).
        if linux.exists():
            print("\n--- Linux CLI control ---")
            t = work / "ctl"
            (t / "src").mkdir(parents=True)
            (t / "src/f").write_text("x")
            run([linux, "c", "c.bp", "src"], t)
            (t / "existing").mkdir()
            r = run([linux, "x", "-o", "existing", "c.bp"], t)
            check(r.returncode == 0 and (t / "existing/src/f").exists(),
                  "the Linux CLI still extracts into an existing directory", r.stderr)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("\nResults: %d passed, %d failed" % (passes, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
