#!/usr/bin/env python3
"""Linux extended attributes, hard links and skipped special files.

Checks that the Linux CLI (a) reports items it cannot archive, (b) stores and
restores extended attributes and POSIX ACLs, with security.* and trusted.* only on
request, and (c) records hard links and recreates them on extraction, refusing a
hard-link record that would join files this run did not extract identically.
Run on a filesystem that accepts user.* attributes (tmpfs does).
"""

import argparse
import os
import shutil
import socket
import stat
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


def run(cmd, cwd=None, env=None):
    full = dict(os.environ)
    full.pop("BOLTPACK_DEBUG", None)
    if env:
        full.update(env)
    return subprocess.run([str(c) for c in cmd], cwd=cwd, env=full, text=True,
                          capture_output=True)


def xattrs(path):
    return {n: os.getxattr(path, n, follow_symlinks=False)
            for n in os.listxattr(path, follow_symlinks=False) if n.startswith("user.")}


def supports_large(directory):
    probe = directory / "probe"
    try:
        probe.write_text("x")
        os.setxattr(probe, "user.p", b"p" * 40000)
        return True
    except OSError:
        return False
    finally:
        try:
            probe.unlink()
        except OSError:
            pass


def pick_workdir():
    for base in ("/dev/shm", tempfile.gettempdir()):
        if os.path.isdir(base):
            d = Path(tempfile.mkdtemp(prefix="bp-lxa-", dir=base))
            if supports_large(d):
                return d
            shutil.rmtree(d, ignore_errors=True)
    return None


def container(items, subject=0, origin=1, version=1):
    """Build a BPNA payload; items is a list of (name bytes, value bytes)."""
    body = b""
    for name, value in items:
        body += (len(name).to_bytes(2, "little") + len(value).to_bytes(4, "little") +
                 name + value)
    return (b"BPNA" + bytes([version, origin, subject, 0]) +
            len(items).to_bytes(2, "little") + body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--linux-cli", default=str(ROOT / "build/linux/boltpack"))
    ap.add_argument("--mini", default=str(ROOT / "build/linux32mini/bpmini"))
    args = ap.parse_args()
    cli = Path(args.linux_cli).resolve()
    work = pick_workdir()
    if work is None:
        print("SKIP: no filesystem here accepts 40 KB user extended attributes")
        return 0
    try:
        # helper that writes archives with a chosen container payload
        objs = []
        for name in ("archiver", "compress", "boltlite", "fluff", "bp_codepage", "boltlz"):
            o = work / (name + ".o")
            subprocess.run([args.cc, "-O2", "-Isrc/core", "-c", "-o", o,
                            ROOT / "src/core" / (name + ".c")], cwd=ROOT, check=True)
            objs.append(str(o))
        helper = work / "make_attr_archive"
        subprocess.run([args.cc, "-O2", "-Isrc/core", "-o", helper,
                        ROOT / "test/macosx_shim/make_attr_archive.c"] + objs,
                       cwd=ROOT, check=True)

        # ---- 1. skipped special files --------------------------------------
        print("\n--- items that cannot be archived are reported ---")
        t = work / "special"
        (t / "src").mkdir(parents=True)
        (t / "src/f.txt").write_text("f")
        os.mkfifo(t / "src/pipe")
        sock = socket.socket(socket.AF_UNIX)
        sock.bind(str(t / "src/sock"))
        r = run([cli, "c", "a.bp", "src"], t)
        check(r.returncode == 0, "create still succeeds (like tar, a warning)", r.stderr)
        check("src/pipe is a FIFO; it is not archived" in r.stderr and
              "src/sock is a socket; it is not archived" in r.stderr,
              "each skipped item is named with its kind", r.stderr)
        check("2 special files" in r.stderr, "the summary counts them", r.stderr)
        check("f.txt" in run([cli, "l", "a.bp"], t).stdout and
              "pipe" not in run([cli, "l", "a.bp"], t).stdout,
              "everything else is archived")
        sock.close()

        # ---- 2. extended attributes ----------------------------------------
        print("\n--- extended attributes ---")
        t = work / "xa"
        (t / "src/sub").mkdir(parents=True)
        (t / "out").mkdir()
        a = t / "src/a.txt"; a.write_text("hello\n")
        big = t / "src/big.bin"; big.write_bytes(b"x" * 100)
        d = t / "src/sub"
        binary = bytes([0, 1, 0xFF, 0x80, 10, 13, 0])
        os.setxattr(a, "user.tag", b"red")
        os.setxattr(a, "user.binary", binary)
        os.setxattr(a, "user.empty", b"")
        os.setxattr(big, "user.max", b"v" * 32768)
        os.setxattr(d, "user.dir", b"directory")
        r = run([cli, "c", "t.bp", "src"], t)
        check(r.returncode == 0 and "native attributes: 5 stored on 3 items" in r.stdout,
              "create stores the attributes and says how many", r.stdout + r.stderr)
        lst = run([cli, "l", "t.bp"], t).stdout
        check(lst.count("ATTR") == 3, "list shows an ATTR entry per item", lst)
        r = run([cli, "x", "-o", "out", "t.bp"], t)
        check(r.returncode == 0 and "native attributes: 5 restored" in r.stdout,
              "extract restores them and says how many", r.stdout + r.stderr)
        check(xattrs(t / "out/src/a.txt") == {"user.tag": b"red", "user.binary": binary,
                                              "user.empty": b""},
              "ordinary, binary and empty values are exact")
        check(xattrs(t / "out/src/big.bin") == {"user.max": b"v" * 32768},
              "a 32 KiB value round trips")
        check(xattrs(t / "out/src/sub") == {"user.dir": b"directory"},
              "directory attributes are restored")
        (t / "src/ro.txt").write_text("r")
        os.setxattr(t / "src/ro.txt", "user.ro", b"readonly")
        os.chmod(t / "src/ro.txt", 0o444)
        run([cli, "c", "ro.bp", "src/ro.txt"], t)
        (t / "out_ro").mkdir()
        r = run([cli, "x", "-o", "out_ro", "ro.bp"], t)
        p = t / "out_ro/src/ro.txt"
        check(r.returncode == 0 and xattrs(p) == {"user.ro": b"readonly"} and
              stat.S_IMODE(p.stat().st_mode) == 0o444,
              "a read-only file gets its attribute and keeps its mode", r.stderr)
        os.chmod(t / "src/ro.txt", 0o644)

        # limits and warnings
        t = work / "lim"
        (t / "src").mkdir(parents=True)
        many = t / "src/many.txt"; many.write_text("m")
        for i in range(70):
            os.setxattr(many, "user.k%02d" % i, b"v")
        over = t / "src/over.txt"; over.write_text("o")
        os.setxattr(over, "user.huge", b"h" * 40000)
        os.setxattr(over, "user.ok", b"fine")
        r = run([cli, "c", "t.bp", "src"], t)
        check(r.returncode == 0 and "values over 32768 bytes are not stored" in r.stderr and
              "not stored: container is full" in r.stderr and
              "7 extended attributes could not be stored" in r.stderr,
              "oversize and surplus attributes are named and counted", r.stderr)
        (t / "out").mkdir()
        run([cli, "x", "-o", "out", "t.bp"], t)
        check(len(xattrs(t / "out/src/many.txt")) == 64 and
              xattrs(t / "out/src/over.txt") == {"user.ok": b"fine"},
              "the rest of each item is restored")

        # ---- 3. ACLs ---------------------------------------------------------
        print("\n--- POSIX ACLs ---")
        # Build a POSIX ACL as its raw xattr value so no tools are needed:
        # version 2, then (tag u16, permissions u16, id u32) entries.
        import struct
        def acl(entries):
            return struct.pack("<I", 2) + b"".join(
                struct.pack("<HHI", tag, perm, ident) for tag, perm, ident in entries)
        t = work / "acl"
        (t / "src/dir").mkdir(parents=True)
        (t / "out").mkdir()
        f = t / "src/f.txt"; f.write_text("x")
        other = os.getuid() + 1 if os.getuid() else 1
        access = acl([(0x01, 6, 0xFFFFFFFF), (0x02, 4, other), (0x04, 4, 0xFFFFFFFF),
                      (0x10, 4, 0xFFFFFFFF), (0x20, 4, 0xFFFFFFFF)])
        default = acl([(0x01, 7, 0xFFFFFFFF), (0x02, 5, other), (0x04, 5, 0xFFFFFFFF),
                       (0x10, 5, 0xFFFFFFFF), (0x20, 5, 0xFFFFFFFF)])
        try:
            os.setxattr(f, "system.posix_acl_access", access)
            os.setxattr(t / "src/dir", "system.posix_acl_default", default)
            have_acl = True
        except OSError as exc:
            have_acl = False
            print("  SKIP  this filesystem refuses POSIX ACLs (%s)" % exc)
        if have_acl:
            r = run([cli, "c", "t.bp", "src"], t)
            check("system.posix_acl_access" not in r.stderr and r.returncode == 0,
                  "an ACL is stored without complaint", r.stderr)
            r = run([cli, "x", "-o", "out", "t.bp"], t)
            def raw(path, name):
                try:
                    return os.getxattr(path, name)
                except OSError:
                    return None
            check(raw(t / "out/src/f.txt", "system.posix_acl_access") ==
                  raw(f, "system.posix_acl_access") is not None,
                  "a file's access ACL round trips", r.stderr)
            check(raw(t / "out/src/dir", "system.posix_acl_default") ==
                  raw(t / "src/dir", "system.posix_acl_default") is not None,
                  "a directory's default ACL round trips", r.stderr)

        # ---- 4. security attributes need an explicit request ---------------
        print("\n--- security attributes ---")
        t = work / "sec"
        t.mkdir()
        (t / "out").mkdir()
        (t / "payload").write_bytes(container([(b"user.ok", b"1"),
                                               (b"security.capability", b"\x01\x00\x00\x02")]))
        run([helper, "t.bp", "prog", "payload", "file", "0xFD"], t)
        r = run([cli, "x", "-o", "out", "t.bp"], t)
        check(r.returncode == 1 and "security attributes are restored only with "
              "--security-attrs" in r.stderr, "restoring security.* is refused unasked",
              r.stderr)
        check(xattrs(t / "out/prog") == {"user.ok": b"1"} and
              (t / "out/prog").read_text() == "data\n",
              "the file and its ordinary attribute are still restored")
        (t / "out2").mkdir()
        r = run([cli, "x", "--security-attrs", "-o", "out2", "t.bp"], t)
        check(r.returncode == 1 and "security.capability" in r.stderr and "errno" in r.stderr,
              "with --security-attrs the attempt is made and its failure reported "
              "(unprivileged here)", r.stderr)

        # ---- 5. malformed and foreign containers --------------------------
        print("\n--- malformed and foreign containers ---")
        cases = [
            ("bad magic", b"XXXX" + container([(b"user.a", b"1")])[4:], "file", "malformed container"),
            ("truncated", container([(b"user.a", b"1")])[:-1], "file", "malformed container"),
            ("newer version", container([(b"user.a", b"1")], version=2), "file",
             "newer than this boltpack understands"),
            ("other platform", container([(b"user.a", b"1")], origin=3), "file", "not Linux"),
            ("directory container on a file", container([(b"user.a", b"1")], subject=4),
             "file", "container is for a directory but the item is a file"),
            ("unknown namespace", container([(b"weird.name", b"1")]), "file",
             "not an attribute this system restores"),
        ]
        for title, payload, kind, expect in cases:
            t = work / ("bad-" + title.replace(" ", "-"))
            t.mkdir(); (t / "out").mkdir()
            (t / "payload").write_bytes(payload)
            run([helper, "t.bp", "item", "payload", kind, "0xFD"], t)
            r = run([cli, "x", "-o", "out", "t.bp"], t)
            check(r.returncode == 1 and expect in r.stderr and
                  (t / "out/item").read_text() == "data\n",
                  title + ": reported, file still extracted", r.stderr)

        # ---- 6. hard links ---------------------------------------------------
        print("\n--- hard links ---")
        t = work / "hl"
        (t / "src/d1").mkdir(parents=True)
        (t / "src/d2").mkdir()
        (t / "out").mkdir()
        (t / "src/d1/one").write_bytes(b"shared content\n" * 100)
        os.link(t / "src/d1/one", t / "src/d2/two")
        os.link(t / "src/d1/one", t / "src/three")
        (t / "src/solo").write_bytes(b"shared content\n" * 100)  # same bytes, own inode
        r = run([cli, "c", "-0", "t.bp", "src"], t)
        check(r.returncode == 0 and "hard links: 2 recorded" in r.stdout,
              "create records the two extra names", r.stdout + r.stderr)
        check(sum(1 for l in run([cli, "l", "t.bp"], t).stdout.splitlines() if " DATA " in l) == 4,
              "every name still has its own data (readers that do not know links get copies)")
        r = run([cli, "x", "-o", "out", "t.bp"], t)
        i1 = (t / "out/src/d1/one").stat()
        check(r.returncode == 0 and "hard links: 2 recreated" in r.stdout and
              i1.st_nlink == 3 and (t / "out/src/d2/two").stat().st_ino == i1.st_ino and
              (t / "out/src/three").stat().st_ino == i1.st_ino,
              "three names share one inode again", r.stdout + r.stderr)
        check((t / "out/src/solo").stat().st_ino != i1.st_ino and
              (t / "out/src/solo").read_bytes() == (t / "out/src/d1/one").read_bytes(),
              "a separate file with equal bytes stays separate")
        # an archive made without the record, and the mini reader, give copies
        if Path(args.mini).exists():
            (t / "outm").mkdir()
            r = run([args.mini, "x", "t.bp", "outm"], t)
            check((t / "outm/src/d2/two").read_bytes() == b"shared content\n" * 100 and
                  (t / "outm/src/d2/two").stat().st_ino != (t / "outm/src/d1/one").stat().st_ino,
                  "the mini reader, which does not know the record, extracts independent copies",
                  r.stderr)
        # deleting the first name leaves records that point at nothing
        shutil.copy(t / "t.bp", t / "d.bp")
        listing = [l.split()[-1] for l in run([cli, "l", "d.bp"], t).stdout.splitlines()
                   if " DATA " in l]
        group = [n for n in listing if n.rsplit("/", 1)[-1] in ("one", "two", "three")]
        run([cli, "d", "d.bp", group[0]], t)
        (t / "outd").mkdir()
        r = run([cli, "x", "-o", "outd", "d.bp"], t)
        survivors = [(t / "outd" / n).read_bytes() for n in group[1:]]
        check(all(b == b"shared content\n" * 100 for b in survivors) and
              "was not extracted; it stays a separate copy" in r.stderr and r.returncode == 1,
              "links whose target was deleted stay copies and are reported", r.stderr)

        # hostile hard-link records
        def hostile(title, target, setup=None, expect="stays a separate copy"):
            h = work / ("hl-" + title.replace(" ", "-"))
            h.mkdir(); (h / "out").mkdir()
            (h / "payload").write_bytes(container([(b"boltpack.hardlink", target)]))
            run([helper, "t.bp", "victim-link", "payload", "file", "0xFD"], h)
            if setup:
                setup(h)
            r = run([cli, "x", "-o", "out", "t.bp"], h)
            check(expect in r.stderr and (h / "out/victim-link").read_text() == "data\n" and
                  (h / "out/victim-link").stat().st_nlink == 1,
                  "hard-link record to %s is refused" % title, r.stderr)
            return h
        hostile("a missing target", b"nowhere")
        hostile("a path outside the tree", b"../escape")
        hostile("itself", b"victim-link")

        def preexisting(h):
            (h / "out/victim").write_text("data\n")   # same size and bytes as the entry
        h = hostile("a file the archive did not extract", b"victim", preexisting)
        check((h / "out/victim").stat().st_nlink == 1,
              "that existing file was not linked to")

        # ---- 7. profiles carry neither --------------------------------------
        print("\n--- profiles and edits ---")
        t = work / "prof"
        (t / "src").mkdir(parents=True)
        (t / "src/a").write_text("a")
        os.setxattr(t / "src/a", "user.k", b"v")
        os.link(t / "src/a", t / "src/b")
        r = run([cli, "c", "--interchange-profile-1", "p.bp", "src"], t)
        check("ATTR" not in run([cli, "l", "p.bp"], t).stdout and "hard links" not in r.stdout,
              "Interchange Profile 1 archives carry no attribute or link record")
        r = run([cli, "c", "e.bp", "src"], t)
        run([cli, "a", "e.bp", "src/b"], t) if False else None
        (t / "extra.txt").write_text("e")
        r = run([cli, "a", "e.bp", "extra.txt"], t)
        check(run([cli, "l", "e.bp"], t).stdout.count("ATTR") == 2,
              "appending keeps the existing attribute entries")
        r = run([cli, "x", "-n", "-o", "nothing", "e.bp"], t)
        check(r.returncode == 0 and not (t / "nothing").exists(), "dry run writes nothing")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\nResults: %d passed, %d failed" % (passes, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
