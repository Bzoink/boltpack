#!/usr/bin/env python3
"""Verify symlink timestamps are no-follow and symlink publication is staged."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]

INTERPOSER = r'''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int (*real_utimensat_fn)(int, const char *, const struct timespec *, int);
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    const char *fail = getenv("BP_TEST_FAIL_SYMLINK_TIME");
    const char *marker = getenv("BP_TEST_SYMLINK_TIME_MARKER");
    int fd;
    if (fail != NULL && (flags & AT_SYMLINK_NOFOLLOW) != 0 &&
        strstr(path, ".boltpack-link-") != NULL) {
        if (marker != NULL) {
            fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) close(fd);
        }
        errno = EIO;
        return -1;
    }
    if (real_utimensat_fn == NULL)
        *(void **)(&real_utimensat_fn) = dlsym(RTLD_NEXT, "utimensat");
    if (real_utimensat_fn == NULL) { errno = EIO; return -1; }
    return real_utimensat_fn(dirfd, path, times, flags);
}
'''


def run(args, cwd, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True)
    if result.returncode != 0:
        raise AssertionError(
            f"command {args!r} returned {result.returncode}\n"
            f"stdout: {result.stdout.decode(errors='replace')}\n"
            f"stderr: {result.stderr.decode(errors='replace')}"
        )
    return result


def main():
    if not sys.platform.startswith("linux"):
        print("SKIP: symlink publication fault injection requires Linux")
        return 0
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    binary = args.boltpack.resolve()
    if not binary.is_file():
        raise SystemExit(f"BoltPack executable not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="bp_symlink_atomic_") as temp:
        root = Path(temp)
        source = root / "src"
        source.mkdir()
        payload = source / "target.txt"
        payload.write_bytes(b"symlink target data\n")
        payload_mtime = 1_600_000_000
        link_mtime = 1_700_000_123
        os.utime(payload, (payload_mtime, payload_mtime))
        link = source / "link.txt"
        link.symlink_to("target.txt")
        os.utime(link, (link_mtime, link_mtime), follow_symlinks=False)

        archive = root / "links.bolt"
        run([str(binary), "c", str(archive), "src"], root)

        output = root / "out"
        run([str(binary), "x", "-o", str(output), str(archive)], root)
        extracted_link = output / "src/link.txt"
        extracted_payload = output / "src/target.txt"
        assert extracted_link.is_symlink()
        assert os.readlink(extracted_link) == "target.txt"
        assert int(os.lstat(extracted_link).st_mtime) == link_mtime, \
            "the symlink mtime was not restored"
        assert int(extracted_payload.stat().st_mtime) == payload_mtime, \
            "setting the symlink mtime changed its target"

        no_force_output = root / "no-force-out"
        (no_force_output / "src").mkdir(parents=True)
        no_force_link = no_force_output / "src/link.txt"
        no_force_link.symlink_to("old-target")
        result = subprocess.run([str(binary), "x", "-o", str(no_force_output),
                                 str(archive)], cwd=root, capture_output=True)
        assert result.returncode != 0, "extraction overwrote an existing symlink"
        assert no_force_link.is_symlink() and os.readlink(no_force_link) == "old-target", \
            "no-force extraction changed the existing symlink"

        # A failure while setting link metadata must not replace an existing
        # link or publish the staged symlink.
        fail_output = root / "force-out"
        (fail_output / "src").mkdir(parents=True)
        existing = fail_output / "src/link.txt"
        existing.symlink_to("old-target")
        old_mtime = 1_500_000_000
        os.utime(existing, (old_mtime, old_mtime), follow_symlinks=False)
        helper = root / "fail-utimensat.c"
        library = root / "fail-utimensat.so"
        helper.write_text(INTERPOSER, encoding="ascii")
        subprocess.run(shlex.split(args.cc) + ["-shared", "-fPIC", str(helper),
                                               "-ldl", "-o", str(library)],
                       check=True, cwd=ROOT)
        marker = root / "utimensat-called"
        env = os.environ.copy()
        prior_preload = env.get("LD_PRELOAD")
        env["LD_PRELOAD"] = str(library) + (":" + prior_preload if prior_preload else "")
        env["BP_TEST_FAIL_SYMLINK_TIME"] = "1"
        env["BP_TEST_SYMLINK_TIME_MARKER"] = str(marker)
        result = subprocess.run([str(binary), "x", "-f", "-o", str(fail_output),
                                 str(archive)], cwd=root, env=env,
                                capture_output=True)
        assert result.returncode != 0, "symlink timestamp failure was reported as success"
        assert marker.exists(), "timestamp failure injection did not reach the staged link"
        assert existing.is_symlink() and os.readlink(existing) == "old-target", \
            "failed metadata restoration replaced the existing symlink"
        assert int(os.lstat(existing).st_mtime) == old_mtime, \
            "failed metadata restoration changed the existing symlink mtime"
        assert not list((fail_output / "src").glob(".boltpack-link-*")), \
            "failed symlink extraction left its stage"

        run([str(binary), "x", "-f", "-o", str(fail_output), str(archive)], root)
        assert os.readlink(existing) == "target.txt", "forced extraction did not replace the link"
        assert int(os.lstat(existing).st_mtime) == link_mtime

    print("Linux symlink timestamp and staged-publication checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
