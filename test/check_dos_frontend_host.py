#!/usr/bin/env python3
"""Exercise full DOS frontend extraction on a POSIX host build.

This validates frontend logic and staged-output failure handling. It does not
replace a build or runtime test with DJGPP/DOS.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile


INTERPOSER = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
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
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
    static size_t (*real_fwrite)(const void *, size_t, size_t, FILE *);
    if (!real_fwrite) real_fwrite = dlsym(RTLD_NEXT, "fwrite");
    if (fileno(stream) == stage_fd && getenv("BP_FAIL_WRITE")) {
        errno = EIO;
        return 0;
    }
    return real_fwrite(ptr, size, count, stream);
}
int fclose(FILE *stream)
{
    static int (*real_fclose)(FILE *);
    int fail;
    if (!real_fclose) real_fclose = dlsym(RTLD_NEXT, "fclose");
    fail = fileno(stream) == stage_fd && getenv("BP_FAIL_CLOSE") != NULL;
    if (fail) stage_fd = -1;
    if (real_fclose(stream) != 0) return EOF;
    if (fail) {
        errno = EIO;
        return EOF;
    }
    return 0;
}
int rename(const char *oldpath, const char *newpath)
{
    static int (*real_rename)(const char *, const char *);
    if (!real_rename) real_rename = dlsym(RTLD_NEXT, "rename");
    if (getenv("BP_FAIL_RENAME") && strstr(oldpath, "BP") &&
        strstr(oldpath, ".TMP")) {
        errno = EIO;
        return -1;
    }
    return real_rename(oldpath, newpath);
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


def primary_mtimes(path):
    archive = path.read_bytes()
    table_offset = struct.unpack_from("<Q", archive, len(archive) - 20)[0]
    count = struct.unpack_from("<I", archive, table_offset + 4)[0]
    record = table_offset + 16
    entries = {}
    for _ in range(count):
        name_length = struct.unpack_from("<H", archive, record)[0]
        name = archive[record + 2:record + 2 + name_length].decode("utf-8")
        fields = record + 2 + name_length
        entry_type = archive[fields + 16]
        mtime = struct.unpack_from("<Q", archive, fields + 18)[0]
        flags = struct.unpack_from("<H", archive, fields + 41)[0]
        if entry_type in (0, 3, 4):
            entries[name] = (mtime, flags)
        record = fields + 43
    return entries


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
        pre_epoch = source / "pre-epoch.txt"
        pre_epoch.write_bytes(b"DOS host pre-epoch timestamp regression\n")
        os.utime(pre_epoch, (-1, -1))
        result = run([str(boltpack), "c", "pre-epoch.blt", pre_epoch.name],
                     source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        archive_times = primary_mtimes(source / "pre-epoch.blt")
        mtime, flags = archive_times[pre_epoch.name]
        assert mtime == 0 and not (flags & 0x4000), \
            "DOS frontend marked a pre-epoch source time as valid"
        assert run([str(boltpack), "t", "pre-epoch.blt"], source).returncode == 0

        readonly_source = source / "src" / "readonly.txt"
        readonly_source.write_bytes(b"read-only portable FAT flag\n")
        readonly_source.chmod(0o444)
        hidden_system_source = source / "src" / ".hidden.SYS"
        hidden_system_source.write_bytes(b"hidden and system name flags\n")
        protected_dir_source = source / "src" / "protected"
        protected_dir_source.mkdir()
        (protected_dir_source / "child.txt").write_bytes(b"child of protected directory\n")
        protected_dir_source.chmod(0o555)
        result = run([str(boltpack), "c", "normal.blt", "src"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")

        output = root / "output"
        result = run([str(boltpack), "x", "-o", str(output), "normal.blt"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        target = output / "src" / "file.txt"
        assert target.read_bytes() == b"verified DOS frontend output\n"
        readonly_target = output / "src" / "readonly.txt"
        assert readonly_target.read_bytes() == b"read-only portable FAT flag\n"
        assert readonly_target.stat().st_mode & 0o222 == 0
        hidden_system_target = output / "src" / ".hidden.SYS"
        assert hidden_system_target.read_bytes() == b"hidden and system name flags\n"
        protected_dir_target = output / "src" / "protected"
        assert (protected_dir_target / "child.txt").read_bytes() == b"child of protected directory\n"
        assert protected_dir_target.stat().st_mode & 0o222 == 0

        # The host simulation maps a directory's FAT read-only flag to POSIX
        # write permissions. Make it writable so the second extraction can
        # create sibling stages, then verify the archive restores the flag.
        protected_dir_target.chmod(0o755)
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

        for index, invalid_name in enumerate((
                "NUL.txt", "NUL .txt", "trailing.", "trailing ",
                "question?.txt", "star*.txt", "pipe|.txt", 'quote".txt',
                "colon:name.txt", "angle<.txt")):
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
            no_lfn_env = os.environ.copy()
            no_lfn_env["BP_DOS_TEST_NO_LFN"] = "1"

            short_source = root / "no_lfn_short_source"
            (short_source / "src").mkdir(parents=True)
            (short_source / "src" / "SHORT.TXT").write_bytes(b"short name")
            result = run([str(linux_boltpack), "c", "--single-thread",
                          "short.blt", "src"], short_source)
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            short_output = "OUT"
            result = run([str(boltpack), "x", "-o", short_output,
                          "short.blt"], short_source, env=no_lfn_env)
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert (short_source / short_output / "src" / "SHORT.TXT").read_bytes() == b"short name"

            long_source = root / "no_lfn_long_source"
            (long_source / "src").mkdir(parents=True)
            (long_source / "src" / "LongerName.txt").write_bytes(b"long name")
            (long_source / "src" / "LONGER~1.TXT").write_bytes(b"literal alias")
            result = run([str(linux_boltpack), "c", "--single-thread",
                          "long-and-alias.blt", "src"], long_source)
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            long_output = "OUT"
            result = run([str(boltpack), "x", "-f", "-o", long_output,
                          "long-and-alias.blt"], long_source, env=no_lfn_env)
            assert result.returncode != 0
            assert b"preflight" in result.stderr.lower()
            assert not (long_source / long_output).exists()

            long_root = "LONGOUTPUT"
            result = run([str(boltpack), "x", "-o", long_root,
                          "short.blt"], short_source, env=no_lfn_env)
            assert result.returncode != 0
            assert b"preflight" in result.stderr.lower()
            assert not (short_source / long_root).exists()

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

        addition = source / "addition.txt"
        addition.write_bytes(b"staged DOS archive edit\n")
        archive = source / "normal.blt"

        def archive_snapshot():
            return archive.read_bytes()

        for command, args_for_command in (
            ("c", ["normal.blt", "src"]),
            ("a", ["normal.blt", "addition.txt"]),
            ("d", ["normal.blt", "src/file.txt"]),
        ):
            before = archive_snapshot()
            env = os.environ.copy()
            env.update(LD_PRELOAD=str(interposer), BP_FAIL_WRITE="1")
            result = run([str(boltpack), command, *args_for_command], source, env=env)
            assert result.returncode != 0, f"{command}: staged write failure was not reported"
            assert archive_snapshot() == before, f"{command}: failed edit changed the original archive"
            assert not list(source.glob("BP*.TMP")), f"{command}: failed edit left a stage"
            assert not list(source.glob("BK*.BAK")), f"{command}: failed edit left a recovery file"

        for command, args_for_command in (
            ("c", ["normal.blt", "src"]),
            ("a", ["normal.blt", "addition.txt"]),
            ("d", ["normal.blt", "src/file.txt"]),
        ):
            before = archive_snapshot()
            env = os.environ.copy()
            env.update(LD_PRELOAD=str(interposer), BP_FAIL_CLOSE="1")
            result = run([str(boltpack), command, *args_for_command], source, env=env)
            assert result.returncode != 0, f"{command}: staged close failure was not reported"
            assert archive_snapshot() == before, f"{command}: close failure changed the original archive"
            assert not list(source.glob("BP*.TMP")), f"{command}: close failure left a stage"
            assert not list(source.glob("BK*.BAK")), f"{command}: close failure left a recovery file"

        for command, args_for_command in (
            ("c", ["normal.blt", "src"]),
            ("a", ["normal.blt", "addition.txt"]),
            ("d", ["normal.blt", "src/file.txt"]),
        ):
            before = archive_snapshot()
            env = os.environ.copy()
            env.update(LD_PRELOAD=str(interposer), BP_FAIL_RENAME="1")
            result = run([str(boltpack), command, *args_for_command], source, env=env)
            assert result.returncode != 0, f"{command}: publication failure was not reported"
            assert archive_snapshot() == before, f"{command}: publication failure changed the original archive"
            assert not list(source.glob("BP*.TMP")), f"{command}: publication failure left a stage"
            assert not list(source.glob("BK*.BAK")), f"{command}: publication failure left a recovery file"

        result = run([str(boltpack), "c", "normal.blt", "src"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        assert run([str(boltpack), "t", "normal.blt"], source).returncode == 0
        result = run([str(boltpack), "a", "normal.blt", "addition.txt"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        listing = run([str(boltpack), "l", "normal.blt"], source)
        assert listing.returncode == 0 and b"addition.txt" in listing.stdout
        assert run([str(boltpack), "t", "normal.blt"], source).returncode == 0
        result = run([str(boltpack), "d", "normal.blt", "addition.txt"], source)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        listing = run([str(boltpack), "l", "normal.blt"], source)
        assert listing.returncode == 0 and b"addition.txt" not in listing.stdout
        assert run([str(boltpack), "t", "normal.blt"], source).returncode == 0
        assert not list(source.glob("BP*.TMP"))
        assert not list(source.glob("BK*.BAK"))

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
