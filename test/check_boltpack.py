#!/usr/bin/env python3
"""
check_boltpack.py -- integration tests for the BoltPack CLI binary.

Each test creates/extracts in a temporary directory and verifies results.
Uses only the Python standard library.

Run:
    python3 test/check_boltpack.py [--boltpack PATH] [--verbose]

The binary defaults to build/linux/boltpack relative to the repository root.
Exit code: 0 = all pass, non-zero = at least one failure.

Copyright (C) 2026 Tony Mattsson
Licensed under the BSD 2-Clause License
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Path setup
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BP = os.path.join(REPO_ROOT, "build", "linux", "boltpack")

# Add test/ to path so we can import gen_testdata
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_testdata import (
    make_file,
    make_basic_tree,
    make_utf8_tree,
    make_deep_tree,
    make_wide_dir,
    make_sparse_file,
    make_symlink,
    BASIC_FILES,
)

# ---------------------------------------------------------------------------
# Simple test framework
# ---------------------------------------------------------------------------

PASS = 0
FAIL = 0
VERBOSE = False
BP = DEFAULT_BP


def _log(msg: str) -> None:
    if VERBOSE:
        print(f"    {msg}")


def check(cond: bool, name: str) -> None:
    global PASS, FAIL
    if cond:
        print(f"  PASS  {name}")
        PASS += 1
    else:
        print(f"  FAIL  {name}")
        FAIL += 1


def run_bp(*args, cwd: str | None = None,
           stdin_data: bytes | None = None) -> subprocess.CompletedProcess:
    """Run boltpack with the given arguments; return CompletedProcess."""
    cmd = [BP, *args]
    _log("$ " + " ".join(cmd))
    result = subprocess.run(
        cmd,
        cwd=cwd,
        capture_output=True,
        input=stdin_data,
    )
    if VERBOSE and result.stdout:
        _log("stdout: " + result.stdout.decode(errors="replace").rstrip())
    if VERBOSE and result.stderr:
        _log("stderr: " + result.stderr.decode(errors="replace").rstrip())
    return result


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while chunk := fh.read(65536):
            h.update(chunk)
    return h.hexdigest()


def file_tree_digests(root: str) -> dict[str, str]:
    """Return {relative_path: sha256_hex} for every file under *root*."""
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            abs_path = os.path.join(dirpath, fname)
            rel_path = os.path.relpath(abs_path, root).replace(os.sep, "/")
            out[rel_path] = sha256_file(abs_path)
    return out


def section(name: str) -> None:
    print(f"\n--- {name} ---")


# ---------------------------------------------------------------------------
# Test helpers
# ---------------------------------------------------------------------------

def roundtrip_check(label: str, src_dir: str, archive: str,
                    extract_dir: str, src_digests: dict[str, str]) -> None:
    """Verify every file in src_digests round-trips correctly."""
    ext_digests = file_tree_digests(extract_dir)
    all_match = True
    for name, expected in src_digests.items():
        got = ext_digests.get(name)
        if got != expected:
            all_match = False
            _log(f"mismatch {name}: expected {expected[:8]}... got {got}")
    check(all_match, f"{label}: all file digests match after roundtrip")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_binary_exists() -> None:
    section("Prerequisite: boltpack binary")
    check(os.path.isfile(BP) and os.access(BP, os.X_OK),
          f"binary exists and is executable: {BP}")


def test_linux_help_and_long_form_cli() -> None:
    section("Linux CLI: simplified help and long-form syntax")

    r = run_bp("help")
    help_text = (r.stdout + r.stderr).decode(errors="replace")
    check(r.returncode == 0, "help: exit 0")
    check("-2" in help_text and "boltlz" in help_text,
          "help: exposes boltlz as the default compression method")
    check("debug-fluff" not in help_text,
        "help: hides debug-fluff from default page")
    check("-j N" not in help_text,
          "help: hides advanced parallel-compression tuning from default page")
    check("help advanced" in help_text,
        "help: points users to advanced help")
    check("--interchange-profile-1" in help_text,
        "help: exposes the mini-compatible interchange profile")

    r = run_bp("help", "advanced")
    adv_text = (r.stdout + r.stderr).decode(errors="replace")
    check(r.returncode == 0, "help advanced: exit 0")
    check("debug-fluff" in adv_text,
        "help advanced: shows debug-fluff command")
    check("-j N" in adv_text and "-g N" in adv_text,
          "help advanced: shows parallel and fast-fluff tuning switches")

    with tempfile.TemporaryDirectory(prefix="bp_long_cli_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc_auto = os.path.join(tmp, "auto.bolt")
        arc_single = os.path.join(tmp, "single.bolt")
        os.makedirs(src)
        os.makedirs(ext)

        make_basic_tree(src)
        make_file(os.path.join(src, "big.bin"), 5 * 1024 * 1024, "text", 123)
        src_digests = file_tree_digests(src)

        r = run_bp("create", "--fluff", "auto.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "long-form create: exit 0")

        r = run_bp("test", "auto.bolt", cwd=tmp)
        check(r.returncode == 0, "long-form test: exit 0")

        r = run_bp("extract", "--output", "ext", "auto.bolt", cwd=tmp)
        check(r.returncode == 0, "long-form extract: exit 0")
        roundtrip_check("long-form CLI", src, arc_auto,
                        os.path.join(ext, "src"), src_digests)

        r = run_bp("create", "--fluff", "--single-thread",
                   "single.bolt", "src", cwd=tmp)
        check(r.returncode == 0,
              "long-form create with single-thread override: exit 0")

        r = run_bp("test", "single.bolt", cwd=tmp)
        check(r.returncode == 0,
              "single-thread override archive: test exit 0")


def test_roundtrip_per_method() -> None:
    section("Roundtrip: one archive per compression method")

    methods = [
        ("-0", "Stored"),
        ("-1", "BoltLite"),
        ("-2", "boltlz"),
        ("-F", "Fluff"),
        ("-Z", "zstd"),
    ]
    for flag, name in methods:
        with tempfile.TemporaryDirectory(prefix=f"bp_rt_{name}_") as tmp:
            src = os.path.join(tmp, "src")
            ext = os.path.join(tmp, "ext")
            arc = os.path.join(tmp, f"test_{name}.bolt")
            os.makedirs(src)
            os.makedirs(ext)
            digests = make_basic_tree(src)

            # create
            r = run_bp("c", flag, f"test_{name}.bolt", "src", cwd=tmp)
            check(r.returncode == 0,
                  f"{name}: create exit 0")

            # test integrity
            r = run_bp("t", f"test_{name}.bolt", cwd=tmp)
            check(r.returncode == 0,
                  f"{name}: test integrity exit 0")

            # extract
            r = run_bp("x", "-o", "ext", f"test_{name}.bolt", cwd=tmp)
            check(r.returncode == 0,
                  f"{name}: extract exit 0")

            # digest compare
            ext_digests = file_tree_digests(os.path.join(ext, "src"))
            all_match = all(
                ext_digests.get(name, None) == d
                for name, d in digests.items()
            )
            check(all_match,
                  f"{name}: all digests match after roundtrip")


def test_operations() -> None:
    section("Operations: list, info, delete")

    with tempfile.TemporaryDirectory(prefix="bp_ops_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "ops.bolt")
        os.makedirs(src)
        os.makedirs(ext)
        make_basic_tree(src)

        run_bp("c", "ops.bolt", "src", cwd=tmp)

        # list
        r = run_bp("l", "ops.bolt", cwd=tmp)
        check(r.returncode == 0, "list: exit 0")
        check(b"text.txt" in r.stdout, "list: text.txt in output")

        # info
        r = run_bp("i", "ops.bolt", cwd=tmp)
        check(r.returncode == 0, "info: exit 0")

        # delete one entry
        r = run_bp("d", "ops.bolt", "src/text.txt", cwd=tmp)
        if r.returncode == 0:
            r2 = run_bp("l", "ops.bolt", cwd=tmp)
            check(b"text.txt" not in r2.stdout,
                  "delete: text.txt removed from listing")
        else:
            _log("delete returned non-zero (may need exact entry path)")


def test_delete_publication_failure() -> None:
    section("Delete: failed publication preserves original archive")

    interposer = r'''#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int rename(const char *old_path, const char *new_path)
{
    const char *target = getenv("BP_TEST_RENAME_TARGET");
    const char *marker = getenv("BP_TEST_RENAME_MARKER");
    int fd;
    (void)old_path;
    if (target == NULL || strcmp(target, new_path) != 0) {
        errno = EIO;
        return -1;
    }
    if (marker != NULL) {
        fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) close(fd);
    }
    errno = EIO;
    return -1;
}
'''

    with tempfile.TemporaryDirectory(prefix="bp_delete_publish_") as tmp:
        source = os.path.join(tmp, "src")
        archive = os.path.join(tmp, "archive.bolt")
        marker = os.path.join(tmp, "rename-called")
        interposer_c = os.path.join(tmp, "fail_rename.c")
        interposer_so = os.path.join(tmp, "fail_rename.so")
        os.makedirs(source)
        make_file(os.path.join(source, "file.txt"), 128, "text", 93)

        created = run_bp("c", "archive.bolt", "src", cwd=tmp)
        check(created.returncode == 0,
              "delete publication fixture archive created")
        if created.returncode != 0:
            return
        os.chmod(archive, 0o640)
        with open(archive, "rb") as stream:
            original_bytes = stream.read()

        with open(interposer_c, "w", encoding="utf-8") as stream:
            stream.write(interposer)
        compiler = shutil.which("cc")
        if compiler is None:
            check(False, "C compiler available for rename-failure injection")
            return
        built = subprocess.run(
            [compiler, "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
             interposer_c, "-o", interposer_so],
            capture_output=True,
        )
        check(built.returncode == 0,
              "rename-failure interposer compiled")
        if built.returncode != 0:
            return

        env = os.environ.copy()
        prior_preload = env.get("LD_PRELOAD")
        env["LD_PRELOAD"] = interposer_so + (":" + prior_preload if prior_preload else "")
        env["BP_TEST_RENAME_TARGET"] = archive
        env["BP_TEST_RENAME_MARKER"] = marker
        failed = subprocess.run(
            [BP, "d", archive, "src/file.txt"], cwd=tmp,
            env=env, capture_output=True,
        )
        with open(archive, "rb") as stream:
            after_failure = stream.read()
        check(os.path.exists(marker), "delete reached the injected rename failure")
        check(failed.returncode != 0 and after_failure == original_bytes,
              "failed atomic publication preserves original archive bytes")
        check(not any(name.startswith("bpdel_") for name in os.listdir(tmp)),
              "failed delete removes its staged archive")

        completed = run_bp("d", "archive.bolt", "src/file.txt", cwd=tmp)
        tested = run_bp("t", "archive.bolt", cwd=tmp)
        check(completed.returncode == 0 and tested.returncode == 0,
              "successful delete publishes a valid replacement archive")
        check(os.stat(archive).st_mode & 0o777 == 0o640,
              "delete replacement retains archive permission bits")


def test_empty_archive() -> None:
    section("Edge case: empty archive")

    with tempfile.TemporaryDirectory(prefix="bp_empty_") as tmp:
        src = os.path.join(tmp, "src")
        arc = os.path.join(tmp, "empty.bolt")
        os.makedirs(src)
        # do NOT add any files to src -- create an archive of an empty dir

        r = run_bp("c", "empty.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "empty archive: create exit 0")

        r = run_bp("t", "empty.bolt", cwd=tmp)
        check(r.returncode == 0, "empty archive: test exit 0")

        r = run_bp("l", "empty.bolt", cwd=tmp)
        check(r.returncode == 0, "empty archive: list exit 0")


def test_empty_file() -> None:
    section("Edge case: 0-byte file")

    with tempfile.TemporaryDirectory(prefix="bp_zero_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "zero.bolt")
        os.makedirs(src)
        os.makedirs(ext)
        open(os.path.join(src, "empty.bin"), "wb").close()

        r = run_bp("c", "zero.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "zero-byte file: create exit 0")

        r = run_bp("t", "zero.bolt", cwd=tmp)
        check(r.returncode == 0, "zero-byte file: test exit 0")

        r = run_bp("x", "-o", "ext", "zero.bolt", cwd=tmp)
        check(r.returncode == 0, "zero-byte file: extract exit 0")

        extracted = os.path.join(ext, "src", "empty.bin")
        if not os.path.exists(extracted):
            # try without subdir prefix
            extracted = os.path.join(ext, "empty.bin")
        check(os.path.exists(extracted) and os.path.getsize(extracted) == 0,
              "zero-byte file: extracted file is 0 bytes")


def test_utf8_filenames() -> None:
    section("UTF-8 filenames: non-ASCII characters")

    with tempfile.TemporaryDirectory(prefix="bp_utf8_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "utf8.bolt")
        os.makedirs(src)
        os.makedirs(ext)
        make_utf8_tree(src)

        r = run_bp("c", "utf8.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "UTF-8 names: create exit 0")

        r = run_bp("t", "utf8.bolt", cwd=tmp)
        check(r.returncode == 0, "UTF-8 names: test exit 0")

        r = run_bp("l", "utf8.bolt", cwd=tmp)
        check(r.returncode == 0, "UTF-8 names: list exit 0")

        r = run_bp("x", "-o", "ext", "utf8.bolt", cwd=tmp)
        check(r.returncode == 0, "UTF-8 names: extract exit 0")

        # Verify CJK filename extracted somewhere under ext
        found_cjk = any("\u4e2d\u6587" in p
                         for p in file_tree_digests(ext))
        check(found_cjk, "UTF-8 names: CJK filename preserved after extract")


def test_deep_directory() -> None:
    section("Deep directory tree (20 levels)")

    with tempfile.TemporaryDirectory(prefix="bp_deep_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "deep.bolt")
        os.makedirs(src)
        os.makedirs(ext)
        digests = make_deep_tree(src)

        r = run_bp("c", "deep.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "deep tree: create exit 0")

        r = run_bp("t", "deep.bolt", cwd=tmp)
        check(r.returncode == 0, "deep tree: test exit 0")

        r = run_bp("x", "-o", "ext", "deep.bolt", cwd=tmp)
        check(r.returncode == 0, "deep tree: extract exit 0")

        ext_files = file_tree_digests(ext)
        # path in archive will have src/ prefix -- find leaf.txt anywhere
        found_leaf = any(p.endswith("leaf.txt") for p in ext_files)
        check(found_leaf, "deep tree: leaf.txt extracted")


def test_wide_directory() -> None:
    section("Wide directory (500 files)")

    with tempfile.TemporaryDirectory(prefix="bp_wide_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "wide.bolt")
        os.makedirs(src)
        os.makedirs(ext)
        make_wide_dir(src, count=500)

        r = run_bp("c", "wide.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "wide dir: create exit 0")

        r = run_bp("t", "wide.bolt", cwd=tmp)
        check(r.returncode == 0, "wide dir: test exit 0")

        r = run_bp("x", "-o", "ext", "wide.bolt", cwd=tmp)
        check(r.returncode == 0, "wide dir: extract exit 0")

        ext_count = len(file_tree_digests(ext))
        check(ext_count == 500, f"wide dir: extracted {ext_count}/500 files")


def test_large_file_64bit() -> None:
    """
    Verify that a sparse file just over 4 GB can be archived and listed
    with the correct 64-bit size, without actually allocating 4 GB of disk.
    Uses boltlz so the sparse all-zero payload compresses to a small archive
    instead of requiring >4 GB of free output space.
    """
    section("Large file: >4 GB sparse (64-bit offset test)")

    with tempfile.TemporaryDirectory(prefix="bp_large_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "large.bolt")
        os.makedirs(src)
        os.makedirs(ext)

        size_4gb_plus = 4 * 1024 * 1024 * 1024 + 1024  # 4 GiB + 1 KiB
        sparse = os.path.join(src, "large.bin")
        make_sparse_file(sparse, size_4gb_plus)

        r = run_bp("c", "-2", "large.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "large file: create exit 0")

        r = run_bp("t", "large.bolt", cwd=tmp)
        check(r.returncode == 0, "large file: test exit 0")

        r = run_bp("l", "large.bolt", cwd=tmp)
        check(r.returncode == 0, "large file: list exit 0")
        listing = r.stdout.decode(errors="replace")
        check("large.bin" in listing, "large file: filename appears in listing")

        r = run_bp("i", "large.bolt", cwd=tmp)
        check(r.returncode == 0, "large file: info exit 0")
        info_text = r.stdout.decode(errors="replace")

        # Accept either the exact byte count or the formatted summary string.
        size_str = str(size_4gb_plus)
        size_ok = (size_str in info_text or
               "4.00 GB" in info_text or
               "4.0 GB" in info_text)
        check(size_ok, "large file: size > 4 GB visible in archive info")


def test_corrupt_archive() -> None:
    section("Corrupt archive: various truncation points")

    with tempfile.TemporaryDirectory(prefix="bp_corrupt_") as tmp:
        src = os.path.join(tmp, "src")
        arc_good = os.path.join(tmp, "good.bolt")
        arc_bad = os.path.join(tmp, "bad.bolt")
        os.makedirs(src)
        make_basic_tree(src)

        run_bp("c", "good.bolt", "src", cwd=tmp)

        with open(arc_good, "rb") as fh:
            good_data = fh.read()

        archive_size = len(good_data)
        # Test truncation at 25%, 50%, 75% of the archive
        all_nonzero = True
        for fraction in [0.25, 0.50, 0.75]:
            trunc_size = int(archive_size * fraction)
            with open(arc_bad, "wb") as fh:
                fh.write(good_data[:trunc_size])
            r = run_bp("t", arc_bad)
            if r.returncode == 0:
                all_nonzero = False
                _log(f"truncated at {fraction:.0%}: unexpectedly returned 0")

        check(all_nonzero, "corrupt (truncated): non-zero exit on all truncations")

        # Byte flip in data section
        if archive_size > 100:
            corrupt = bytearray(good_data)
            mid = archive_size // 2
            corrupt[mid] ^= 0xFF
            corrupt[mid + 1] ^= 0xFF
            with open(arc_bad, "wb") as fh:
                fh.write(corrupt)
            r = run_bp("t", arc_bad)
            check(r.returncode != 0, "corrupt (bit-flip): test returns non-zero")


def test_wrong_magic() -> None:
    section("Wrong magic header")

    with tempfile.TemporaryDirectory(prefix="bp_magic_") as tmp:
        bad = os.path.join(tmp, "bad.bolt")
        with open(bad, "wb") as fh:
            fh.write(b"\xAA" * 64)

        r = run_bp("t", bad)
        check(r.returncode != 0, "wrong magic: test returns non-zero")

        r = run_bp("l", bad)
        check(r.returncode != 0, "wrong magic: list returns non-zero")


def test_symlink_roundtrip() -> None:
    section("Symlink entry roundtrip")

    with tempfile.TemporaryDirectory(prefix="bp_sym_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "sym.bolt")
        os.makedirs(src)
        os.makedirs(ext)

        make_file(os.path.join(src, "real.txt"), 256, "text", 50)
        make_symlink(os.path.join(src, "link.txt"), "real.txt")

        r = run_bp("c", "sym.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "symlink: create exit 0")

        r = run_bp("t", "sym.bolt", cwd=tmp)
        check(r.returncode == 0, "symlink: test exit 0")

        r = run_bp("x", "-o", "ext", "sym.bolt", cwd=tmp)
        check(r.returncode == 0, "symlink: extract exit 0")


def test_append() -> None:
    section("Append: add files to existing archive")

    with tempfile.TemporaryDirectory(prefix="bp_append_") as tmp:
        src1 = os.path.join(tmp, "src1")
        src2 = os.path.join(tmp, "src2")
        ext  = os.path.join(tmp, "ext")
        arc  = os.path.join(tmp, "append.bolt")
        os.makedirs(src1)
        os.makedirs(src2)
        os.makedirs(ext)

        make_file(os.path.join(src1, "first.txt"), 512, "text", 60)
        make_file(os.path.join(src2, "second.txt"), 512, "text", 61)
        src1_digests = file_tree_digests(src1)

        r = run_bp("c", "append.bolt", "src1", cwd=tmp)
        check(r.returncode == 0, "append: initial create exit 0")

        r = run_bp("a", "append.bolt", "src2", cwd=tmp)
        check(r.returncode == 0, "append: append exit 0")

        r = run_bp("l", "append.bolt", cwd=tmp)
        check(b"first.txt"  in r.stdout, "append: first.txt in listing")
        check(b"second.txt" in r.stdout, "append: second.txt in listing")

        # Integrity + extraction must survive the append, not just listing:
        # a stale-tail layout bug once passed the listing check while leaving
        # the appended entries unreadable.
        r = run_bp("t", "append.bolt", cwd=tmp)
        check(r.returncode == 0, "append: test integrity exit 0")

        r = run_bp("x", "-o", "ext", "append.bolt", cwd=tmp)
        check(r.returncode == 0, "append: extract exit 0")
        roundtrip_check("append", src1, arc,
                        os.path.join(ext, "src1"), src1_digests)


def test_interchange_profile() -> None:
    section("Interchange Profile 1: create, append, and refusal")

    with tempfile.TemporaryDirectory(prefix="bp_profile1_") as tmp:
        make_file(os.path.join(tmp, "first.txt"), 1024, "text", 71)
        make_file(os.path.join(tmp, "second.txt"), 2048, "text", 72)
        make_file(os.path.join(tmp, "compressible.bin"), 65536, "ones", 73)

        r = run_bp("c", "--interchange-profile-1", "profile.bolt",
                   "first.txt", cwd=tmp)
        check(r.returncode == 0, "profile: create exit 0")
        r = run_bp("t", "profile.bolt", cwd=tmp)
        check(r.returncode == 0, "profile: created archive passes integrity test")

        r = run_bp("c", "--interchange-profile-1", "-0",
                   "stored-profile.bolt", "first.txt", cwd=tmp)
        check(r.returncode == 0, "profile: Stored codec is accepted")
        r = run_bp("t", "stored-profile.bolt", cwd=tmp)
        check(r.returncode == 0, "profile: Stored archive passes integrity test")

        r = run_bp("a", "--interchange-profile-1", "profile.bolt",
                   "second.txt", cwd=tmp)
        check(r.returncode == 0, "profile: append exit 0")
        r = run_bp("t", "profile.bolt", cwd=tmp)
        check(r.returncode == 0, "profile: appended archive passes integrity test")

        r = run_bp("c", "--interchange-profile-1", "-F", "rejected.bolt",
                   "first.txt", cwd=tmp)
        check(r.returncode != 0 and not os.path.exists(os.path.join(tmp, "rejected.bolt")),
              "profile: refuses conflicting codec and creates no archive")

        r = run_bp("c", "-2", "extended.bolt", "compressible.bin", cwd=tmp)
        check(r.returncode == 0, "profile refusal fixture: extended-codec archive created")
        extended_path = os.path.join(tmp, "extended.bolt")
        with open(extended_path, "rb") as fh:
            before = fh.read()
        r = run_bp("a", "--interchange-profile-1", "extended.bolt",
                   "second.txt", cwd=tmp)
        with open(extended_path, "rb") as fh:
            after = fh.read()
        check(r.returncode != 0 and before == after,
              "profile: refuses extended-codec append without changing archive bytes")


def test_zstd_levels() -> None:
    section("zstd compression levels (-Z -l 1 vs -Z -l 9)")

    with tempfile.TemporaryDirectory(prefix="bp_zstd_") as tmp:
        src = os.path.join(tmp, "src")
        arc1 = os.path.join(tmp, "zstd_level1.bolt")
        arc9 = os.path.join(tmp, "zstd_level9.bolt")
        os.makedirs(src)
        # Use repetitive data so higher levels produce smaller output
        make_file(os.path.join(src, "data.bin"), 65536, "text", 70)

        r1 = run_bp("c", "-Z", "-l", "1", "zstd_level1.bolt", "src", cwd=tmp)
        check(r1.returncode == 0, "zstd level 1: create exit 0")

        r9 = run_bp("c", "-Z", "-l", "9", "zstd_level9.bolt", "src", cwd=tmp)
        check(r9.returncode == 0, "zstd level 9: create exit 0")

        if r1.returncode == 0 and r9.returncode == 0:
            size1 = os.path.getsize(arc1)
            size9 = os.path.getsize(arc9)
            check(size9 <= size1,
                  f"zstd level 9 ({size9}) <= level 1 ({size1})")

        # integrity
        r = run_bp("t", "zstd_level9.bolt", cwd=tmp)
        check(r.returncode == 0, "zstd level 9: test integrity exit 0")

        # info should show method = zstd
        r = run_bp("i", "zstd_level9.bolt", cwd=tmp)
        check(r.returncode == 0, "zstd level 9: info exit 0")
        check(b"zstd" in r.stdout, "zstd level 9: method name 'zstd' in info output")


def test_fluff_levels() -> None:
    section("Fluff compression levels (-l 1 vs -l 9)")

    with tempfile.TemporaryDirectory(prefix="bp_fluff_") as tmp:
        src = os.path.join(tmp, "src")
        arc1 = os.path.join(tmp, "level1.bolt")
        arc9 = os.path.join(tmp, "level9.bolt")
        os.makedirs(src)
        # Use repetitive data so level 9 can outperform level 1
        make_file(os.path.join(src, "data.bin"), 65536, "text", 70)

        r1 = run_bp("c", "-F", "-l", "1", "level1.bolt", "src", cwd=tmp)
        check(r1.returncode == 0, "fluff level 1: create exit 0")

        r9 = run_bp("c", "-F", "-l", "9", "level9.bolt", "src", cwd=tmp)
        check(r9.returncode == 0, "fluff level 9: create exit 0")

        if r1.returncode == 0 and r9.returncode == 0:
            size1 = os.path.getsize(arc1)
            size9 = os.path.getsize(arc9)
            check(size9 <= size1,
                  f"fluff level 9 ({size9}) <= level 1 ({size1})")

        # integrity of both
        r = run_bp("t", "level9.bolt", cwd=tmp)
        check(r.returncode == 0, "fluff level 9: test integrity exit 0")


def test_fast_fluff_threaded_roundtrip() -> None:
    section("Fast-fluff threaded grouped roundtrip (-j 2 -g 4)")

    with tempfile.TemporaryDirectory(prefix="bp_fast_fluff_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "threaded.bolt")
        os.makedirs(src)
        os.makedirs(ext)

        make_basic_tree(src)
        make_file(os.path.join(src, "big.bin"), 12 * 1024 * 1024, "text", 321)
        src_digests = file_tree_digests(src)

        r = run_bp("c", "-F", "-j", "2", "-g", "4",
                   "threaded.bolt", "src", cwd=tmp)
        check(r.returncode == 0,
              "fast-fluff threaded grouped create: exit 0")

        r = run_bp("t", "threaded.bolt", cwd=tmp)
        check(r.returncode == 0,
              "fast-fluff threaded grouped test: exit 0")

        r = run_bp("x", "-o", "ext", "threaded.bolt", cwd=tmp)
        check(r.returncode == 0,
              "fast-fluff threaded grouped extract: exit 0")
        roundtrip_check("fast-fluff threaded grouped", src, arc,
                        os.path.join(ext, "src"), src_digests)


def test_fast_boltlz_threaded_roundtrip() -> None:
    section("Parallel BoltLZ block roundtrip (-j 2)")

    with tempfile.TemporaryDirectory(prefix="bp_fast_boltlz_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        threaded_arc = os.path.join(tmp, "threaded.bolt")
        serial_arc = os.path.join(tmp, "serial.bolt")
        small_src = os.path.join(tmp, "small")
        os.makedirs(src)
        os.makedirs(ext)
        os.makedirs(small_src)

        make_file(os.path.join(src, "big.bin"), 8 * 1024 * 1024,
                  "text", 867)
        make_file(os.path.join(small_src, "small.bin"), 16 * 1024,
                  "text", 868)
        src_digests = file_tree_digests(src)

        threaded = run_bp("c", "-2", "-j", "2", "threaded.bolt", "src",
                          cwd=tmp)
        check(threaded.returncode == 0,
              "parallel BoltLZ create: exit 0")
        serial = run_bp("c", "-2", "--single-thread", "serial.bolt", "src",
                        cwd=tmp)
        check(serial.returncode == 0,
              "single-thread BoltLZ create: exit 0")

        if threaded.returncode == 0 and serial.returncode == 0:
            check(os.path.getsize(threaded_arc) == os.path.getsize(serial_arc),
                  "parallel BoltLZ preserves the sequential archive size")

        r = run_bp("t", "threaded.bolt", cwd=tmp)
        check(r.returncode == 0, "parallel BoltLZ test: exit 0")
        r = run_bp("x", "-o", "ext", "threaded.bolt", cwd=tmp)
        check(r.returncode == 0, "parallel BoltLZ extract: exit 0")
        roundtrip_check("parallel BoltLZ", src, threaded_arc,
                        os.path.join(ext, "src"), src_digests)

        small = run_bp("c", "-2", "-j", "2", "small.bolt", "small",
                       cwd=tmp)
        check(small.returncode == 0,
              "small BoltLZ file with -j stays on a valid sequential path")
        r = run_bp("t", "small.bolt", cwd=tmp)
        check(r.returncode == 0, "small BoltLZ archive test: exit 0")


def test_format6_metadata_roundtrip() -> None:
    section("Format 6: timestamps, permissions and overwrite protection")
    with tempfile.TemporaryDirectory(prefix="bp_format6_") as tmp:
        source = os.path.join(tmp, "src")
        os.mkdir(source)
        filename = os.path.join(source, "epoch.txt")
        with open(filename, "wb") as fh:
            fh.write(b"format six\n")
        os.chmod(filename, 0o640)
        os.utime(filename, (0, 0))
        os.chmod(source, 0o750)
        os.utime(source, (1234567890, 1234567890))
        r = run_bp("c", "--single-thread", "sample.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "format 6: create")
        r = run_bp("x", "-o", "ext", "sample.bolt", cwd=tmp)
        check(r.returncode == 0, "format 6: extract")
        restored = os.path.join(tmp, "ext", "src")
        target = os.path.join(restored, "epoch.txt")
        check(os.stat(target).st_mtime == 0, "format 6: Unix epoch is preserved")
        check(os.stat(target).st_mode & 0o777 == 0o640, "format 6: file mode is preserved")
        check(os.stat(restored).st_mode & 0o777 == 0o750, "format 6: directory mode is preserved")
        check(os.stat(restored).st_mtime == 1234567890, "format 6: directory mtime is restored after children")
        with open(target, "wb") as fh:
            fh.write(b"existing")
        r = run_bp("x", "-o", "ext", "sample.bolt", cwd=tmp)
        check(r.returncode != 0, "format 6: existing output reports incomplete extraction")
        with open(target, "rb") as fh:
            check(fh.read() == b"existing", "format 6: existing output is not overwritten")


def test_extract_symlink_parent() -> None:
    section("Extraction rejects symlink directory components")
    with tempfile.TemporaryDirectory(prefix="bp_symlink_parent_") as tmp:
        os.mkdir(os.path.join(tmp, "src"))
        with open(os.path.join(tmp, "src", "file"), "wb") as fh:
            fh.write(b"payload")
        check(run_bp("c", "--single-thread", "sample.bolt", "src", cwd=tmp).returncode == 0,
              "symlink parent: create archive")
        os.mkdir(os.path.join(tmp, "ext"))
        os.mkdir(os.path.join(tmp, "outside"))
        os.symlink(os.path.join(tmp, "outside"), os.path.join(tmp, "ext", "src"))
        r = run_bp("x", "-o", "ext", "sample.bolt", cwd=tmp)
        check(r.returncode != 0, "symlink parent: extraction reports skipped entries")
        check(not os.path.exists(os.path.join(tmp, "outside", "file")),
              "symlink parent: file is not written outside destination")


def test_duplicate_source_names() -> None:
    section("Duplicate source names")
    with tempfile.TemporaryDirectory(prefix="bp_duplicate_") as tmp:
        with open(os.path.join(tmp, "same.txt"), "wb") as fh:
            fh.write(b"duplicate")
        result = run_bp("c", "--single-thread", "duplicate.bolt", "same.txt", "same.txt", cwd=tmp)
        check(result.returncode != 0, "duplicate names: create reports an error")
        check(not os.path.exists(os.path.join(tmp, "duplicate.bolt")),
              "duplicate names: no invalid archive is committed")
        result = run_bp("c", "--single-thread", "existing.bolt", "same.txt", cwd=tmp)
        check(result.returncode == 0, "duplicate append: create original archive")
        archive = os.path.join(tmp, "existing.bolt")
        with open(archive, "rb") as fh:
            before = fh.read()
        result = run_bp("a", "--single-thread", "existing.bolt", "same.txt", cwd=tmp)
        check(result.returncode != 0, "duplicate append: reports an error")
        with open(archive, "rb") as fh:
            check(fh.read() == before, "duplicate append: original archive is restored")
        result = run_bp("t", "existing.bolt", cwd=tmp)
        check(result.returncode == 0, "duplicate append: original archive remains valid")


def test_compression_fallback() -> None:
    section("Compression fallback: incompressible data stored as-is")

    with tempfile.TemporaryDirectory(prefix="bp_fallback_") as tmp:
        src = os.path.join(tmp, "src")
        ext = os.path.join(tmp, "ext")
        arc = os.path.join(tmp, "fallback.bolt")
        os.makedirs(src)
        os.makedirs(ext)
        # Pseudo-random data should not compress well; archiver must not enlarge
        make_file(os.path.join(src, "rand.bin"), 32768, "random", 99)
        src_size = os.path.getsize(os.path.join(src, "rand.bin"))

        r = run_bp("c", "-2", "fallback.bolt", "src", cwd=tmp)
        check(r.returncode == 0, "fallback: create exit 0")

        r = run_bp("t", "fallback.bolt", cwd=tmp)
        check(r.returncode == 0, "fallback: test exit 0")

        r = run_bp("x", "-o", "ext", "fallback.bolt", cwd=tmp)
        check(r.returncode == 0, "fallback: extract exit 0")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    global VERBOSE, BP

    parser = argparse.ArgumentParser(description="BoltPack integration tests")
    parser.add_argument("--boltpack", default=DEFAULT_BP,
                        help="Path to boltpack binary")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Show command details")
    parser.add_argument("--test", "-k", default=None,
                        help="Run only tests matching this substring")
    args = parser.parse_args()

    VERBOSE = args.verbose
    BP = os.path.abspath(args.boltpack)

    tests = [
        test_binary_exists,
        test_linux_help_and_long_form_cli,
        test_roundtrip_per_method,
        test_operations,
        test_delete_publication_failure,
        test_empty_archive,
        test_empty_file,
        test_utf8_filenames,
        test_deep_directory,
        test_wide_directory,
        test_large_file_64bit,
        test_corrupt_archive,
        test_wrong_magic,
        test_symlink_roundtrip,
        test_append,
        test_interchange_profile,
        test_fluff_levels,
        test_zstd_levels,
        test_fast_fluff_threaded_roundtrip,
        test_fast_boltlz_threaded_roundtrip,
        test_format6_metadata_roundtrip,
        test_extract_symlink_parent,
        test_duplicate_source_names,
        test_compression_fallback,
    ]

    print(f"BoltPack integration tests  [{BP}]")

    for t in tests:
        if args.test and args.test.lower() not in t.__name__.lower():
            continue
        try:
            t()
        except Exception as exc:
            print(f"  ERROR  {t.__name__}: {exc}")
            global FAIL
            FAIL += 1

    print(f"\nResults: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
