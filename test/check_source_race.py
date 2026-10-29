#!/usr/bin/env python3
"""Reject source mutations and path substitutions during archive creation."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


INTERPOSER = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int bp_mutate_fd = -1;
static int bp_mutate_after_close_fd = -1;

static void bp_coarsen_stat(struct stat *st)
{
    if (st == NULL || !getenv("BP_TEST_COARSE_STAT")) return;
    st->st_mtim.tv_sec = 1700000000;
    st->st_mtim.tv_nsec = 0;
    st->st_ctim.tv_sec = 1700000000;
    st->st_ctim.tv_nsec = 0;
}

int lstat(const char *path, struct stat *st)
{
    static int (*real_lstat)(const char *, struct stat *);
    int rc;
    if (!real_lstat) real_lstat = dlsym(RTLD_NEXT, "lstat");
    rc = real_lstat(path, st);
    if (rc == 0) bp_coarsen_stat(st);
    return rc;
}

int fstat(int fd, struct stat *st)
{
    static int (*real_fstat)(int, struct stat *);
    int rc;
    if (!real_fstat) real_fstat = dlsym(RTLD_NEXT, "fstat");
    rc = real_fstat(fd, st);
    if (rc == 0) bp_coarsen_stat(st);
    return rc;
}

FILE *fopen(const char *path, const char *mode)
{
    static FILE *(*real_fopen)(const char *, const char *);
    static int swapped;
    static int resized;
    static int chmodded;
    const char *target = getenv("BP_TEST_SOURCE");
    const char *parked = getenv("BP_TEST_PARKED");
    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen");
    if (!chmodded && target && getenv("BP_TEST_CHMOD_BEFORE_OPEN") &&
        strcmp(path, target) == 0 && strcmp(mode, "rb") == 0) {
        chmodded = 1;
        if (chmod(target, 0600) != 0) return NULL;
    }
    if (!resized && target && getenv("BP_TEST_GROW_BEFORE_OPEN") &&
        strcmp(path, target) == 0 && strcmp(mode, "rb") == 0) {
        int fd = open(target, O_WRONLY);
        resized = 1;
        if (fd < 0 || pwrite(fd, "replacement!!!", 14, 0) != 14 ||
            ftruncate(fd, 14) != 0) {
            if (fd >= 0) close(fd);
            return NULL;
        }
        close(fd);
    }
    if (!swapped && target && parked && strcmp(path, target) == 0 &&
        strcmp(mode, "rb") == 0) {
        int fd;
        swapped = 1;
        if (rename(target, parked) != 0) return NULL;
        fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) return NULL;
        if (write(fd, "replacement!!", 13) != 13) {
            close(fd);
            return NULL;
        }
        close(fd);
    }
    {
        FILE *stream = real_fopen(path, mode);
        if (stream != NULL && target && strcmp(path, target) == 0 &&
            strcmp(mode, "rb") == 0) {
            if (getenv("BP_TEST_MUTATE_DURING_READ"))
                bp_mutate_fd = fileno(stream);
            if (getenv("BP_TEST_MUTATE_AFTER_CLOSE"))
                bp_mutate_after_close_fd = fileno(stream);
        }
        return stream;
    }
}

int fclose(FILE *stream)
{
    static int (*real_fclose)(FILE *);
    int mutate;
    int rc;
    const char *target = getenv("BP_TEST_SOURCE");
    if (!real_fclose) real_fclose = dlsym(RTLD_NEXT, "fclose");
    mutate = fileno(stream) == bp_mutate_after_close_fd &&
             getenv("BP_TEST_MUTATE_AFTER_CLOSE") != NULL;
    rc = real_fclose(stream);
    if (mutate && target) {
        int fd = open(target, O_WRONLY);
        if (fd >= 0) {
            (void)pwrite(fd, "replacement!!", 13, 0);
            close(fd);
        }
        bp_mutate_after_close_fd = -1;
    }
    return rc;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream)
{
    static size_t (*real_fread)(void *, size_t, size_t, FILE *);
    static int mutated;
    size_t n;
    if (!real_fread) real_fread = dlsym(RTLD_NEXT, "fread");
    n = real_fread(ptr, size, count, stream);
    if (!mutated && getenv("BP_TEST_MUTATE_DURING_READ") &&
        fileno(stream) == bp_mutate_fd) {
        const char *target = getenv("BP_TEST_SOURCE");
        int fd = target ? open(target, O_WRONLY) : -1;
        mutated = 1;
        if (fd >= 0) {
            (void)pwrite(fd, "replacement!!", 13, 0);
            close(fd);
        }
    }
    return n;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    static int (*real_openat)(int, const char *, int, ...);
    static int swapped;
    const char *target = getenv("BP_TEST_SOURCE");
    int fd;
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    if (flags & O_CREAT) {
        va_list ap;
        mode_t mode;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        fd = real_openat(dirfd, path, flags, mode);
    } else {
        fd = real_openat(dirfd, path, flags);
    }
    if (fd >= 0 && !swapped && getenv("BP_TEST_SYMLINK_TRANSIENT") &&
        target && strcmp(path, target) == 0 && (flags & O_PATH) != 0 &&
        (flags & O_NOFOLLOW) != 0) {
        swapped = 1;
        if (renameat(dirfd, path, dirfd, "saved-link") != 0 ||
            symlinkat("substituted-target", dirfd, path) != 0 ||
            renameat(dirfd, path, dirfd, "substituted-link") != 0 ||
            renameat(dirfd, "saved-link", dirfd, path) != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    static int (*real_fstatat)(int, const char *, struct stat *, int);
    static int swapped;
    static int symlink_swapped;
    int rc;
    const char *parent = getenv("BP_TEST_PARENT");
    const char *parked = getenv("BP_TEST_PARKED_DIR");
    const char *outside = getenv("BP_TEST_OUTSIDE");
    if (!real_fstatat) real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    rc = real_fstatat(dirfd, path, st, flags);
    if (!symlink_swapped && rc == 0 &&
        getenv("BP_TEST_SYMLINK_NAME") &&
        strcmp(path, getenv("BP_TEST_SYMLINK_NAME")) == 0 &&
        (flags & AT_SYMLINK_NOFOLLOW) != 0) {
        const char *replacement = getenv("BP_TEST_SYMLINK_REPLACEMENT");
        symlink_swapped = 1;
        if (renameat(dirfd, path, dirfd, "saved-link") != 0 ||
            symlinkat(replacement, dirfd, path) != 0)
            return -1;
    }
    if (!swapped && rc == 0 && parent && parked && outside &&
        strcmp(path, "source.bin") == 0 &&
        (flags & AT_SYMLINK_NOFOLLOW) != 0) {
        swapped = 1;
        if (rename(parent, parked) != 0 || symlink(outside, parent) != 0)
            return -1;
    }
    return rc;
}
'''


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_race_") as temp:
        root = Path(temp)
        source = root / "source.bin"
        archive = root / "archive.blt"
        parked = root / "source.original"
        source.write_bytes(b"original data")
        created = subprocess.run([str(binary), "c", archive.name, source.name],
                                 cwd=root, capture_output=True, text=True)
        assert created.returncode == 0, created.stderr + created.stdout
        original_archive_hash = digest(archive)

        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_SOURCE=source.name,
                   BP_TEST_PARKED=parked.name)
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert parked.read_bytes() == b"original data"
        assert source.read_bytes() == b"replacement!!"
        assert result.returncode != 0, "source pathname substitution was accepted"
        assert "source changed during archive creation" in result.stderr, result.stderr
        assert digest(archive) == original_archive_hash, \
            "failed append changed the existing archive"
        leftovers = [p.name for p in root.iterdir()
                     if p.name.startswith(".boltpack-") or p.name.startswith("BPK")]
        assert not leftovers, f"failed append left staged files: {leftovers}"


def run_directory_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_dir_race_") as temp:
        root = Path(temp)
        archive = root / "archive.blt"
        seed = root / "seed.bin"
        source_dir = root / "src"
        outside = root / "outside"
        parked = root / "src.original"
        source_dir.mkdir()
        outside.mkdir()
        seed.write_bytes(b"archive seed")
        (source_dir / "source.bin").write_bytes(b"original data")
        (outside / "source.bin").write_bytes(b"replacement!!")
        created = subprocess.run([str(binary), "c", archive.name, seed.name],
                                 cwd=root, capture_output=True, text=True)
        assert created.returncode == 0, created.stderr + created.stdout
        original_archive_hash = digest(archive)

        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_PARENT=source_dir.name,
                   BP_TEST_PARKED_DIR=parked.name, BP_TEST_OUTSIDE=outside.name)
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source_dir.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert parked.is_dir() and not parked.is_symlink()
        assert (parked / "source.bin").read_bytes() == b"original data"
        assert source_dir.is_symlink() and source_dir.resolve() == outside
        assert result.returncode != 0, "source directory substitution was accepted"
        assert "source changed during archive creation" in result.stderr, result.stderr
        assert digest(archive) == original_archive_hash, \
            "failed append changed the existing archive"


def run_inplace_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_inplace_race_") as temp:
        root = Path(temp)
        source = root / "source.bin"
        archive = root / "archive.blt"
        source.write_bytes(b"original data")
        subprocess.run([str(binary), "c", archive.name, source.name],
                       cwd=root, capture_output=True, check=True)
        original_archive_hash = digest(archive)
        original_inode = source.stat().st_ino

        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_SOURCE=source.name,
                   BP_TEST_MUTATE_DURING_READ="1")
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert result.returncode != 0, "in-place source mutation was accepted"
        assert "source changed while archiving" in result.stderr, result.stderr
        assert source.stat().st_ino == original_inode
        assert source.read_bytes() == b"replacement!!"
        assert digest(archive) == original_archive_hash, \
            "failed append changed the existing archive"


def run_coarse_size_change_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_coarse_size_") as temp:
        root = Path(temp)
        source = root / "source.bin"
        archive = root / "archive.blt"
        source.write_bytes(b"original data")
        subprocess.run([str(binary), "c", archive.name, source.name],
                       cwd=root, capture_output=True, check=True)
        original_archive_hash = digest(archive)
        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_SOURCE=source.name,
                   BP_TEST_COARSE_STAT="1", BP_TEST_GROW_BEFORE_OPEN="1")
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert source.read_bytes() == b"replacement!!!"
        assert result.returncode != 0, \
            "source resize with coarse timestamps was accepted"
        assert "source changed during archive creation" in result.stderr, \
            result.stderr
        assert digest(archive) == original_archive_hash, \
            "coarse-timestamp source resize changed the existing archive"
        leftovers = [p.name for p in root.iterdir()
                     if p.name.startswith(".boltpack-") or p.name.startswith("BPK")]
        assert not leftovers, f"coarse-timestamp failure left stages: {leftovers}"


def run_coarse_mode_change_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_coarse_mode_") as temp:
        root = Path(temp)
        source = root / "source.bin"
        archive = root / "archive.blt"
        source.write_bytes(b"original data")
        source.chmod(0o644)
        subprocess.run([str(binary), "c", archive.name, source.name],
                       cwd=root, capture_output=True, check=True)
        original_archive_hash = digest(archive)
        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_SOURCE=source.name,
                   BP_TEST_COARSE_STAT="1", BP_TEST_CHMOD_BEFORE_OPEN="1")
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert source.read_bytes() == b"original data"
        assert (source.stat().st_mode & 0o7777) == 0o600
        assert result.returncode != 0, \
            "source mode change with coarse timestamps was accepted"
        assert "source changed during archive creation" in result.stderr, \
            result.stderr
        assert digest(archive) == original_archive_hash, \
            "coarse-timestamp source mode change changed the existing archive"
        leftovers = [p.name for p in root.iterdir()
                     if p.name.startswith(".boltpack-") or p.name.startswith("BPK")]
        assert not leftovers, f"coarse-timestamp mode failure left stages: {leftovers}"


def run_after_read_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_after_read_") as temp:
        root = Path(temp)
        source = root / "source.bin"
        archive = root / "archive.blt"
        source.write_bytes(b"original data")
        subprocess.run([str(binary), "c", archive.name, source.name],
                       cwd=root, capture_output=True, check=True)
        original_archive_hash = digest(archive)
        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_SOURCE=source.name,
                   BP_TEST_MUTATE_AFTER_CLOSE="1")
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert source.read_bytes() == b"replacement!!"
        assert result.returncode != 0, "post-read source mutation was accepted"
        assert "source changed during archive creation" in result.stderr, result.stderr
        assert digest(archive) == original_archive_hash, \
            "post-read source mutation changed the existing archive"
        leftovers = [p.name for p in root.iterdir()
                     if p.name.startswith(".boltpack-") or p.name.startswith("BPK")]
        assert not leftovers, f"post-read mutation left staged files: {leftovers}"


def run_symlink_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_symlink_race_") as temp:
        root = Path(temp)
        archive = root / "archive.blt"
        seed = root / "seed.bin"
        source_dir = root / "src"
        source_dir.mkdir()
        seed.write_bytes(b"archive seed")
        link = source_dir / "link"
        link.symlink_to("original-target")
        created = subprocess.run([str(binary), "c", archive.name, seed.name],
                                 cwd=root, capture_output=True, text=True)
        assert created.returncode == 0, created.stderr + created.stdout
        original_archive_hash = digest(archive)
        original_archive_hash = digest(archive)

        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_SYMLINK_NAME=link.name,
                   BP_TEST_SYMLINK_REPLACEMENT="substituted-target")
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source_dir.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert (source_dir / "saved-link").is_symlink()
        assert os.readlink(source_dir / "saved-link") == "original-target"
        assert os.readlink(link) == "substituted-target"
        assert result.returncode != 0, "source symlink substitution was accepted"
        assert "source symlink changed during enumeration" in result.stderr, \
            result.stderr
        assert digest(archive) == original_archive_hash, \
            "failed append changed the existing archive"


def run_symlink_transient_case(binary: Path, parallel: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="bp_source_symlink_transient_") as temp:
        root = Path(temp)
        archive = root / "archive.blt"
        seed = root / "seed.bin"
        source = root / "source-link"
        seed.write_bytes(b"archive seed")
        source.symlink_to("original-target")
        created = subprocess.run([str(binary), "c", archive.name, seed.name],
                                 cwd=root, capture_output=True, text=True)
        assert created.returncode == 0, created.stderr + created.stdout
        original_archive_hash = digest(archive)

        interposer_c = root / "interpose.c"
        interposer_so = root / "interpose.so"
        interposer_c.write_text(INTERPOSER)
        subprocess.run(["cc", "-shared", "-fPIC", str(interposer_c),
                        "-ldl", "-o", str(interposer_so)],
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(interposer_so), BP_TEST_SOURCE=source.name,
                   BP_TEST_SYMLINK_TRANSIENT="1")
        command = [str(binary), "a"]
        if parallel:
            command += ["-F", "-j", "2"]
        command += [archive.name, source.name]
        result = subprocess.run(command, cwd=root, env=env,
                                capture_output=True, text=True)
        assert result.returncode != 0, "transient source symlink mutation was accepted"
        assert "source symlink changed during enumeration" in result.stderr, \
            result.stderr
        assert os.readlink(source) == "original-target"
        assert digest(archive) == original_archive_hash, \
            "failed append changed the existing archive"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", type=Path, required=True)
    parser.add_argument("--parallel-boltpack", type=Path)
    args = parser.parse_args()
    run_case(args.boltpack.resolve(), False)
    run_directory_case(args.boltpack.resolve(), False)
    run_inplace_case(args.boltpack.resolve(), False)
    run_coarse_size_change_case(args.boltpack.resolve(), False)
    run_coarse_mode_change_case(args.boltpack.resolve(), False)
    run_after_read_case(args.boltpack.resolve(), False)
    run_symlink_case(args.boltpack.resolve(), False)
    run_symlink_transient_case(args.boltpack.resolve(), False)
    if args.parallel_boltpack is not None:
        run_case(args.parallel_boltpack.resolve(), True)
        run_directory_case(args.parallel_boltpack.resolve(), True)
        run_inplace_case(args.parallel_boltpack.resolve(), True)
        run_coarse_size_change_case(args.parallel_boltpack.resolve(), True)
        run_coarse_mode_change_case(args.parallel_boltpack.resolve(), True)
        run_after_read_case(args.parallel_boltpack.resolve(), True)
        run_symlink_case(args.parallel_boltpack.resolve(), True)
        run_symlink_transient_case(args.parallel_boltpack.resolve(), True)
    print("Source checks reject substitutions, writes, and coarse-time size/mode changes")


if __name__ == "__main__":
    main()
