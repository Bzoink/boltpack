#!/usr/bin/env python3
"""Exercise extraction races and staged-output failure cleanup.

Requires Linux, a C compiler, and a dynamically linked target. The interposer
makes path races, write failures, and close failures deterministic; all paths
are in a temporary directory. A 32-bit target can run through --runner.
"""
import os
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BP = ROOT / "build/linux/boltpack"

INTERPOSER = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
static int fail_fd = -1;
static int fail_close_fd = -1;
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
    static size_t (*real_fwrite)(const void *, size_t, size_t, FILE *);
    const char *marker;
    int marker_fd;
    if (!real_fwrite) real_fwrite = dlsym(RTLD_NEXT, "fwrite");
    if (fail_fd >= 0 && fileno(stream) == fail_fd) {
        marker = getenv("BP_TEST_MARKER");
        if (marker) {
            marker_fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (marker_fd >= 0) close(marker_fd);
        }
        errno = EIO;
        return 0;
    }
    return real_fwrite(ptr, size, count, stream);
}
int fclose(FILE *stream)
{
    static int (*real_fclose)(FILE *);
    int fd = fileno(stream);
    int fail = fd == fail_close_fd;
    int rc;
    if (!real_fclose) real_fclose = dlsym(RTLD_NEXT, "fclose");
    rc = real_fclose(stream);
    if (fail) {
        const char *marker = getenv("BP_TEST_MARKER");
        int marker_fd;
        if (marker) {
            marker_fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (marker_fd >= 0) close(marker_fd);
        }
        errno = EIO;
        return EOF;
    }
    return rc;
}
int openat(int dirfd, const char *name, int flags, ...)
{
    static int (*real_openat)(int, const char *, int, ...);
    static int swapped;
    mode_t mode = 0;
    int fd;
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
        fd = real_openat(dirfd, name, flags, mode);
    } else fd = real_openat(dirfd, name, flags);
    if (fd >= 0 && !swapped && (flags & O_CREAT) &&
        strncmp(name, ".boltpack-", 10) == 0) {
        const char *create_name = getenv("BP_TEST_CREATE_NAME");
        const char *parent = getenv("BP_TEST_PARENT");
        const char *parked = getenv("BP_TEST_PARKED");
        const char *outside = getenv("BP_TEST_OUTSIDE");
        const char *marker = getenv("BP_TEST_MARKER");
        int marker_fd;
        swapped = 1;
        if (getenv("BP_TEST_FAIL_WRITE")) {
            fail_fd = fd;
        } else if (getenv("BP_TEST_FAIL_CLOSE")) {
            fail_close_fd = fd;
        } else if (create_name && marker) {
            int created = real_openat(dirfd, create_name, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (created >= 0) {
                (void)write(created, "existing", 8);
                close(created);
                marker_fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (marker_fd >= 0) close(marker_fd);
            }
        } else if (parent && parked && outside && marker && rename(parent, parked) == 0 &&
                   symlink(outside, parent) == 0) {
            marker_fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (marker_fd >= 0) close(marker_fd);
        }
    }
    return fd;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", type=Path, default=BP)
    parser.add_argument("--mini", action="store_true", help="use mini command syntax")
    parser.add_argument("--runner", default=os.environ.get("BP_TEST_RUNNER", ""),
                        help="command prefix for the target binary, e.g. qemu-i386")
    args = parser.parse_args()
    binary = args.boltpack.resolve()
    runner = shlex.split(args.runner)

    def run_target(command, **kwargs):
        return subprocess.run([*runner, *command], **kwargs)

    with tempfile.TemporaryDirectory(prefix="bp_extract_race_") as tmp:
        root = Path(tmp)
        source = root / "src"
        source.mkdir()
        (source / "file.txt").write_bytes(b"anchored output\n")
        create_cmd = ([str(binary), "c", "archive.blt", "src"] if args.mini else
                      [str(binary), "c", "--single-thread", "archive.blt", "src"])
        run_target(create_cmd, cwd=root, capture_output=True, check=True)
        out = root / "out"
        outside = root / "outside"
        out.mkdir()
        outside.mkdir()
        helper = root / "interpose.c"
        library = root / "interpose.so"
        helper.write_text(INTERPOSER)
        with binary.open("rb") as fh:
            elf_header = fh.read(5)
        compiler = ["cc"]
        if elf_header == b"\x7fELF\x01":
            compiler.append("-m32")
        subprocess.run([*compiler, "-shared", "-fPIC", str(helper), "-ldl", "-o", str(library)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(library), BP_TEST_PARENT=str(out / "src"),
                   BP_TEST_PARKED=str(out / "parked"), BP_TEST_OUTSIDE=str(outside),
                   BP_TEST_MARKER=str(root / "swapped"))
        extract_cmd = ([str(binary), "x", "archive.blt", str(out)] if args.mini else
                       [str(binary), "x", "-o", str(out), "archive.blt"])
        result = run_target(extract_cmd, cwd=root, env=env, capture_output=True)
        assert (root / "swapped").exists(), "interposer did not trigger"
        assert result.returncode != 0, "replaced directory was not reported"
        assert not (outside / "file.txt").exists(), "output escaped through replacement symlink"
        assert (out / "parked/file.txt").read_bytes() == b"anchored output\n"

        race_out = root / "race-out"
        race_out.mkdir()
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(library), BP_TEST_CREATE_NAME="file.txt",
                   BP_TEST_MARKER=str(root / "created"))
        extract_cmd = ([str(binary), "x", "archive.blt", str(race_out)] if args.mini else
                       [str(binary), "x", "-o", str(race_out), "archive.blt"])
        result = run_target(extract_cmd, cwd=root, env=env, capture_output=True)
        assert (root / "created").exists(), "no-clobber interposer did not trigger"
        assert result.returncode != 0, "concurrent destination was not reported"
        assert (race_out / "src/file.txt").read_bytes() == b"existing"
        assert not list((race_out / "src").glob(".boltpack-*")), "temporary file remains"

        failure_out = root / "failure-out"
        failure_out.mkdir()
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(library), BP_TEST_FAIL_WRITE="1",
                   BP_TEST_MARKER=str(root / "write-failed"))
        extract_cmd = ([str(binary), "x", "archive.blt", str(failure_out)] if args.mini else
                       [str(binary), "x", "-o", str(failure_out), "archive.blt"])
        result = run_target(extract_cmd, cwd=root, env=env, capture_output=True)
        assert (root / "write-failed").exists(), "write-failure interposer did not trigger"
        assert result.returncode != 0, "write failure was not reported"
        assert not (failure_out / "src/file.txt").exists(), "failed output was committed"
        assert not list(failure_out.rglob(".boltpack-*")), "failed temporary file remains"

        close_out = root / "close-out"
        close_out.mkdir()
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(library), BP_TEST_FAIL_CLOSE="1",
                   BP_TEST_MARKER=str(root / "close-failed"))
        extract_cmd = ([str(binary), "x", "archive.blt", str(close_out)] if args.mini else
                       [str(binary), "x", "-o", str(close_out), "archive.blt"])
        result = run_target(extract_cmd, cwd=root, env=env, capture_output=True)
        assert (root / "close-failed").exists(), "close-failure interposer did not trigger"
        assert result.returncode != 0, "close failure was not reported"
        assert not (close_out / "src/file.txt").exists(), "close-failed output was committed"
        assert not list(close_out.rglob(".boltpack-*")), "close-failed temporary file remains"
        print("Extraction resists path races and cleans up write/close failures")


if __name__ == "__main__":
    main()
