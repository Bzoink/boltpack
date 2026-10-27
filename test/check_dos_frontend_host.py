#!/usr/bin/env python3
"""Exercise full DOS frontend extraction on a POSIX host build.

This validates frontend logic and staged-output failure handling. It does not
replace a build or runtime test with DJGPP/DOS.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


INTERPOSER = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int stage_fd = -1;
int open(const char *path, int flags, ...)
{
    static int (*real_open)(const char *, int, ...);
    int fd;
    mode_t mode = 0;
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        fd = real_open(path, flags, mode);
    } else {
        fd = real_open(path, flags);
    }
    if (fd >= 0 && strstr(path, "BP") && strstr(path, ".TMP"))
        stage_fd = fd;
    return fd;
}
ssize_t write(int fd, const void *buf, size_t count)
{
    static ssize_t (*real_write)(int, const void *, size_t);
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    if (fd == stage_fd && getenv("BP_FAIL_WRITE")) {
        errno = EIO;
        return -1;
    }
    return real_write(fd, buf, count);
}
int close(int fd)
{
    static int (*real_close)(int);
    int fail;
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    fail = fd == stage_fd && getenv("BP_FAIL_CLOSE") != NULL;
    if (fd == stage_fd) stage_fd = -1;
    if (real_close(fd) != 0) return -1;
    if (fail) {
        errno = EIO;
        return -1;
    }
    return 0;
}
'''


def run(command, cwd, **kwargs):
    return subprocess.run(command, cwd=cwd, capture_output=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True, type=Path)
    parser.add_argument("--linux-boltpack", type=Path)
    args = parser.parse_args()
    boltpack = args.boltpack.resolve()

    with tempfile.TemporaryDirectory(prefix="bp_dos_frontend_") as temp:
        root = Path(temp)
        source = root / "source"
        (source / "src").mkdir(parents=True)
        (source / "src" / "file.txt").write_bytes(b"verified DOS frontend output\n")
        result = run([str(boltpack), "c", "normal.blt", "src"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")

        output = root / "output"
        result = run([str(boltpack), "x", "-o", str(output), "normal.blt"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        target = output / "src" / "file.txt"
        assert target.read_bytes() == b"verified DOS frontend output\n"

        target.write_bytes(b"previous destination survives failed replacement\n")
        result = run([str(boltpack), "x", "-f", "-o", str(output), "normal.blt"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        assert target.read_bytes() == b"verified DOS frontend output\n"
        assert not list((output / "src").glob("BP*.TMP"))
        assert not list((output / "src").glob("BK*.BAK"))

        collision = root / "collision"
        (collision / "src" / "Foo").mkdir(parents=True)
        (collision / "src" / "foo").mkdir()
        (collision / "src" / "Foo" / "a.txt").write_bytes(b"A")
        (collision / "src" / "foo" / "b.txt").write_bytes(b"B")
        result = run([str(boltpack), "c", "case.blt", "src"], collision)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        collision_output = collision / "must_not_exist"
        result = run([str(boltpack), "x", "-o", str(collision_output), "case.blt"], collision)
        assert result.returncode != 0
        assert b"preflight" in result.stderr.lower()
        assert not collision_output.exists()

        for index, invalid_name in enumerate(("NUL.txt", "NUL .txt", "trailing.", "trailing ")):
            invalid_source = root / ("invalid_" + str(index))
            (invalid_source / "src").mkdir(parents=True)
            (invalid_source / "src" / invalid_name).write_bytes(b"invalid DOS name")
            archive_name = "invalid.blt"
            result = run([str(boltpack), "c", archive_name, "src"], invalid_source)
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            invalid_output = invalid_source / "must_not_exist"
            result = run([str(boltpack), "x", "-o", str(invalid_output), archive_name], invalid_source)
            assert result.returncode != 0
            assert b"preflight" in result.stderr.lower()
            assert not invalid_output.exists()

        if args.linux_boltpack is not None:
            linux_boltpack = args.linux_boltpack.resolve()
            unicode_source = root / "unicode_source"
            (unicode_source / "src").mkdir(parents=True)
            (unicode_source / "src" / "é.txt").write_bytes(b"lowercase")
            (unicode_source / "src" / "É.txt").write_bytes(b"uppercase")
            result = run([str(linux_boltpack), "c", "--single-thread",
                          "unicode.blt", "src"], unicode_source)
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            unicode_output = unicode_source / "must_not_exist"
            result = run([str(boltpack), "x", "-o", str(unicode_output),
                          "unicode.blt"], unicode_source)
            assert result.returncode != 0
            assert b"preflight" in result.stderr.lower()
            assert not unicode_output.exists()

            unrepresentable_source = root / "unrepresentable_source"
            (unrepresentable_source / "src").mkdir(parents=True)
            (unrepresentable_source / "src" / "Ж.txt").write_bytes(b"outside CP850")
            result = run([str(linux_boltpack), "c", "--single-thread",
                          "unrepresentable.blt", "src"], unrepresentable_source)
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            unrepresentable_output = unrepresentable_source / "must_not_exist"
            result = run([str(boltpack), "x", "-o", str(unrepresentable_output),
                          "unrepresentable.blt"], unrepresentable_source)
            assert result.returncode != 0
            assert not unrepresentable_output.exists()

        interposer_c = root / "failure.c"
        interposer_c.write_text(INTERPOSER)
        interposer = root / "failure.so"
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c), "-ldl", "-o", str(interposer)],
                       check=True, capture_output=True)

        write_output = root / "write_failure"
        write_output.mkdir()
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer), BP_FAIL_WRITE="1")
        result = run([str(boltpack), "x", "-o", str(write_output), "normal.blt"], source, env=env)
        assert result.returncode != 0
        assert not (write_output / "src" / "file.txt").exists()
        assert not list(write_output.rglob("BP*.TMP"))

        close_output = root / "close_failure"
        (close_output / "src").mkdir(parents=True)
        close_target = close_output / "src" / "file.txt"
        close_target.write_bytes(b"old data remains after failed close\n")
        env.pop("BP_FAIL_WRITE")
        env["BP_FAIL_CLOSE"] = "1"
        result = run([str(boltpack), "x", "-f", "-o", str(close_output), "normal.blt"], source, env=env)
        assert result.returncode != 0
        assert close_target.read_bytes() == b"old data remains after failed close\n"
        assert not list(close_output.rglob("BP*.TMP"))
        assert not list(close_output.rglob("BK*.BAK"))

    print("full DOS frontend host extraction checks passed")


if __name__ == "__main__":
    main()
