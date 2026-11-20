#!/usr/bin/env python3
"""Run the macOS CLI's native attribute container code on a Linux host.

The macOS frontend is compiled with -D__APPLE__ against test/macosx_shim, which
backs the Apple extended-attribute calls with Linux user.* attributes.  This
exercises BoltPack's own create/extract logic for the BPNA container
(extended attributes, resource fork entries, Finder info), its limits, its
error reporting, and how other readers treat the entries.  It does not model
macOS itself: kernel behaviour, HFS+/APFS limits and real resource forks need
a Mac.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ["archiver", "compress", "boltlite", "fluff", "bp_codepage", "boltlz"]

failures = []
passes = 0


def check(condition, name, detail=""):
    global passes
    if condition:
        passes += 1
        print("  PASS  " + name)
    else:
        failures.append(name)
        print("  FAIL  " + name + (("  [" + detail + "]") if detail else ""))


def run(cmd, env=None, cwd=None, **kw):
    full = dict(os.environ)
    full.pop("BOLTPACK_DEBUG", None)
    full.pop("BOLTPACK_SHIM_FAIL", None)
    if env:
        full.update(env)
    return subprocess.run([str(c) for c in cmd], env=full, cwd=cwd,
                          capture_output=True, text=True, **kw)


def build(cc, outdir, legacy_at=False):
    outdir.mkdir(parents=True, exist_ok=True)
    common = ["-Isrc/core"]
    objs = []
    for name in CORE:
        src = ROOT / "src/core" / (name + ".c")
        obj = outdir / (name + ".o")
        subprocess.run([cc, "-O2"] + common + ["-c", "-o", obj, src],
                       cwd=ROOT, check=True)
        objs.append(str(obj))
    main_o = outdir / "main.o"
    subprocess.run([cc, "-ansi", "-pedantic", "-Wall", "-Wextra",
                    "-Wno-unused-function", "-O2", "-U__linux__", "-D__APPLE__",
                    "-D_GNU_SOURCE", "-DBP_VERSION_MAJOR=1",
                    "-DBP_VERSION_MINOR=10"] +
                   (["-DBP_FORCE_LEGACY_AT"] if legacy_at else []) +
                   ["-Isrc/core",
                    "-Itest/macosx_shim", "-Itest/macosx_stubs", "-c", "-o",
                    main_o, "src/frontend/macosx/main.c"], cwd=ROOT,
                   check=True, stderr=subprocess.DEVNULL)
    shim_o = outdir / "shim.o"
    subprocess.run([cc, "-O2", "-c", "-o", shim_o,
                    ROOT / "test/macosx_shim/apple_shim.c"], cwd=ROOT,
                   check=True)
    mac = outdir / "boltpack_mac"
    subprocess.run([cc, "-o", mac, main_o, shim_o] + objs, check=True)
    helper = outdir / "make_attr_archive"
    subprocess.run([cc, "-O2", "-Isrc/core", "-o", helper,
                    ROOT / "test/macosx_shim/make_attr_archive.c"] + objs,
                   cwd=ROOT, check=True)
    return mac, helper


def xattrs(path):
    out = {}
    for name in os.listxattr(path, follow_symlinks=False):
        if name.startswith("user."):
            out[name[5:]] = os.getxattr(path, name, follow_symlinks=False)
    return out


def listing(mac, archive):
    return run([mac, "l", archive]).stdout


def supports_large_xattr(directory):
    """True when directory accepts user.* values above the 32 KiB item cap."""
    probe = directory / "probe"
    try:
        probe.write_text("x")
        os.setxattr(probe, "user.probe", b"p" * 40000)
        return True
    except OSError:
        return False
    finally:
        try:
            probe.unlink()
        except OSError:
            pass


def pick_workdir(requested):
    """Use the requested directory when it can hold large extended attribute
    values, otherwise fall back to /dev/shm or the system temp directory."""
    candidates = []
    if requested:
        path = Path(requested)
        shutil.rmtree(path, ignore_errors=True)
        path.mkdir(parents=True)
        candidates.append(path)
    for base in ("/dev/shm", tempfile.gettempdir()):
        if os.path.isdir(base):
            candidates.append(Path(tempfile.mkdtemp(prefix="boltpack-xattr-", dir=base)))
    for path in candidates:
        if supports_large_xattr(path):
            return path
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--linux-boltpack", default=str(ROOT / "build/linux/boltpack"))
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--legacy-at", action="store_true",
                    help="build with the pre-10.10 fchdir emulation of openat() "
                         "and friends (what Mac OS X 10.5 uses)")
    args = ap.parse_args()

    work = pick_workdir(args.workdir)
    if work is None:
        print("SKIP: no filesystem here accepts 40 KB user extended attributes "
              "(ext4 without ea_inode limits them to one block; tmpfs works)")
        return 0
    print("work directory: %s" % work)

    mac, helper = build(args.cc, work / "bin", args.legacy_at)
    print("variant: %s" % ("legacy *at emulation" if args.legacy_at else "native *at calls"))
    linux = Path(args.linux_boltpack).resolve()

    # ---- 1. round trip ---------------------------------------------------
    print("\n--- round trip of files and directories ---")
    t = work / "rt"
    (t / "src/sub").mkdir(parents=True)
    (t / "out").mkdir()
    a = t / "src/a.txt"; a.write_text("hello\n")
    b = t / "src/sub/b.txt"; b.write_text("world\n")
    big = t / "src/big.bin"; big.write_bytes(b"x" * 100)
    binary = bytes([0, 1, 2, 0xFF, 0, 0x80, 10, 13])
    os.setxattr(a, "user.com.apple.metadata:kMDItemUserTags", b"Red")
    os.setxattr(a, "user.com.apple.metadata:kMDItemWhereFroms", b"http://e/x")
    os.setxattr(a, "user.com.example.binary", binary)
    os.setxattr(a, "user.com.example.empty", b"")
    os.setxattr(a, "user.com.apple.ResourceFork", b"RSRCDATA")
    os.setxattr(a, "user.com.apple.macl", b"system")
    os.setxattr(big, "user.com.example.max", b"v" * 32768)
    os.setxattr(b, "user.com.example.fileattr", "ünïcode ✓".encode())
    os.setxattr(t / "src/sub", "user.com.apple.metadata:kMDItemComment",
                b"dir comment")
    r = run([mac, "c", "t.bp", "src"], cwd=t)
    check(r.returncode == 0, "create succeeds", r.stderr)
    check("native attributes: 7 stored on 4 items" in r.stdout,
          "create reports how many attributes were stored", r.stdout)
    check(r.stderr == "", "create prints no warnings for a clean tree", r.stderr)
    lst = listing(mac, t / "t.bp")
    check(lst.count("ATTR") == 4, "list labels one ATTR entry per subject", lst)
    check("ATTR  src/sub/" in lst, "the directory has its own ATTR entry")
    r = run([mac, "x", "-o", "out", "t.bp"], cwd=t)
    check(r.returncode == 0, "extract succeeds", r.stderr)
    check("native attributes: 7 restored" in r.stdout,
          "extract reports how many attributes were restored", r.stdout)
    got = xattrs(t / "out/src/a.txt")
    check(got.get("com.apple.metadata:kMDItemUserTags") == b"Red" and
          got.get("com.apple.metadata:kMDItemWhereFroms") == b"http://e/x",
          "ordinary attributes round trip")
    check(got.get("com.example.binary") == binary,
          "a binary value with NUL and 0xFF bytes is exact")
    check(got.get("com.example.empty") == b"", "an empty value survives")
    check(got.get("com.apple.ResourceFork") == b"RSRCDATA",
          "the resource fork still travels in its own entry")
    check("com.apple.macl" not in got, "system-managed attributes are not carried")
    check(xattrs(t / "out/src/big.bin").get("com.example.max") == b"v" * 32768,
          "a 32 KiB value round trips")
    check(xattrs(t / "out/src/sub/b.txt").get("com.example.fileattr") ==
          "ünïcode ✓".encode(), "UTF-8 value bytes are exact")
    check(xattrs(t / "out/src/sub").get(
          "com.apple.metadata:kMDItemComment") == b"dir comment",
          "directory attributes are restored")
    check((t / "out/src/a.txt").read_text() == "hello\n",
          "file data is unchanged")

    # ---- 2. limits and warnings ------------------------------------------
    print("\n--- limits and create-side warnings ---")
    t = work / "lim"
    (t / "src").mkdir(parents=True)
    f = t / "src/many.txt"; f.write_text("m")
    for i in range(70):
        os.setxattr(f, "user.k%02d" % i, b"v")
    o = t / "src/over.txt"; o.write_text("o")
    os.setxattr(o, "user.com.example.huge", b"h" * 40000)
    os.setxattr(o, "user.com.example.ok", b"fine")
    r = run([mac, "c", "t.bp", "src"], cwd=t)
    check(r.returncode == 0, "create still succeeds when attributes are skipped",
          r.stderr)
    check("attribute 'com.example.huge'" in r.stderr and
          "values over 32768 bytes are not stored" in r.stderr,
          "an oversize value is named in a warning", r.stderr)
    check("not stored: container is full" in r.stderr,
          "attributes past the 64-item limit are reported", r.stderr)
    check("7 native attributes could not be stored" in r.stderr,
          "the summary counts the skipped attributes", r.stderr)
    (t / "out").mkdir()
    r = run([mac, "x", "-o", "out", "t.bp"], cwd=t)
    check(r.returncode == 0 and len(xattrs(t / "out/src/many.txt")) == 64,
          "64 attributes are restored for the file that hit the limit")
    check(xattrs(t / "out/src/over.txt") == {"com.example.ok": b"fine"},
          "the other attribute of the oversize file is kept")

    # ---- 3. restore failure reporting ------------------------------------
    print("\n--- restore failure reporting ---")
    t = work / "fail"
    (t / "src").mkdir(parents=True)
    (t / "out").mkdir()
    f = t / "src/f.txt"; f.write_text("data")
    os.setxattr(f, "user.com.example.good", b"1")
    os.setxattr(f, "user.com.example.fail", b"2")
    r0 = run([mac, "c", "t.bp", "src"], cwd=t)
    r = run([mac, "x", "-o", "out", "t.bp"], cwd=t,
            env={"BOLTPACK_SHIM_FAIL": "com.example.fail"})
    check(r.returncode == 1, "a failed attribute makes extraction exit 1")
    check("cannot set attribute 'com.example.fail'" in r.stderr and
          "src/f.txt" in r.stderr and "errno 1" in r.stderr and
          "not permitted" in r.stderr,
          "the message names the attribute, item, reason and errno", r.stderr)
    check("1 native attribute could not be restored" in r.stderr,
          "the summary reports the failure count", r.stderr)
    check((t / "out/src/f.txt").read_text() == "data" and
          xattrs(t / "out/src/f.txt") == {"com.example.good": b"1"},
          "the data and the other attributes are still restored")

    # ---- 4. debug trace ----------------------------------------------------
    print("\n--- BOLTPACK_DEBUG trace ---")
    t = work / "dbg"
    (t / "src").mkdir(parents=True)
    (t / "out").mkdir()
    f = t / "src/f.txt"; f.write_text("data")
    os.setxattr(f, "user.com.example.one", b"1")
    os.setxattr(f, "user.com.apple.macl", b"x")
    r = run([mac, "c", "t.bp", "src"], cwd=t, env={"BOLTPACK_DEBUG": "1"})
    check("stored 'com.example.one'" in r.stderr and
          "'com.apple.macl' is system managed" in r.stderr,
          "create traces stored and skipped attributes", r.stderr)
    r = run([mac, "x", "-o", "out", "t.bp"], cwd=t, env={"BOLTPACK_DEBUG": "1"})
    check("restored 'com.example.one'" in r.stderr, "extract traces restores",
          r.stderr)

    # ---- 5. Finder info shared with the Mac OS 9 frontend -----------------
    print("\n--- Finder info (Classic Mac metadata) ---")
    t = work / "fi"
    (t / "src").mkdir(parents=True)
    (t / "out").mkdir()
    f = t / "src/doc.txt"; f.write_text("d")
    finder = b"TEXTttxt" + bytes([0x21, 0x00]) + b"\0" * 22
    os.setxattr(f, "user.com.apple.FinderInfo", finder)
    run([mac, "c", "t.bp", "src"], cwd=t)
    lst = listing(mac, t / "t.bp")
    check(any(l.split()[0] == "10" and "META" in l and "doc.txt" in l
              for l in lst.splitlines()),
          "the Classic Mac record is the 10-byte layout Mac OS 9 reads", lst)
    r = run([mac, "x", "-o", "out", "t.bp"], cwd=t)
    check(xattrs(t / "out/src/doc.txt").get("com.apple.FinderInfo") == finder,
          "type, creator and flags round trip exactly")
    check(r.returncode == 0 and r.stderr == "",
          "restoring Finder info is not reported as partial", r.stderr)

    def classic(payload, name):
        d = work / ("classic-" + name)
        d.mkdir(); (d / "out").mkdir()
        (d / "payload").write_bytes(payload)
        run([helper, "t.bp", "item", "payload", "file", "0x02"], cwd=d)
        r2 = run([mac, "x", "-o", "out", "t.bp"], cwd=d)
        return r2, xattrs(d / "out/item").get("com.apple.FinderInfo")

    # Mac OS 9 writes type, creator and big-endian flags (10 bytes).
    r, got = classic(b"APPLMOSS" + bytes([0x40, 0x01]), "os9")
    check(got is not None and got[:10] == b"APPLMOSS" + bytes([0x40, 0x01]) and
          r.returncode == 0,
          "a record written by the Mac OS 9 frontend is applied unchanged",
          r.stderr)
    # Older macOS CLI archives: little-endian flags plus two dates (18 bytes).
    r, got = classic(b"TEXTttxt" + bytes([0x01, 0x40]) + b"\0" * 8, "old")
    check(got is not None and got[:10] == b"TEXTttxt" + bytes([0x40, 0x01]),
          "an older 18-byte record is still read (flags byte order converted)")
    check(r.returncode == 1 and "Classic creation date is not restored" in r.stderr,
          "an older record's ignored dates are explained", r.stderr)
    r, got = classic(b"TEXTttx", "short")
    check(got is None and r.returncode == 1 and "malformed record" in r.stderr,
          "a record of any other size is refused and reported", r.stderr)

    # ---- 6. malformed and foreign containers -----------------------------
    print("\n--- malformed and foreign containers ---")

    def good_payload(subject=0, origin=3, version=1, count=1):
        name, value = b"k", b"v"
        body = (len(name).to_bytes(2, "little") +
                len(value).to_bytes(4, "little") + name + value)
        return (b"BPNA" + bytes([version, origin, subject, 0]) +
                count.to_bytes(2, "little") + body)

    cases = [
        ("bad magic", b"XXXX" + good_payload()[4:], "file",
         "malformed container"),
        ("truncated", good_payload()[:-1], "file", "malformed container"),
        ("newer version", good_payload(version=2), "file",
         "newer than this boltpack understands"),
        ("other platform", good_payload(origin=1), "file", "not macOS"),
        ("directory container on a file", good_payload(subject=4), "file",
         "container is for a directory but the item is a file"),
        ("file container on a directory", good_payload(subject=0), "dir",
         "container is for a file but the item is a directory"),
        ("too many items declared", good_payload(count=3), "file",
         "malformed container"),
        ("oversize container", b"BPNA" + b"\0" * 70000, "file",
         "container size"),
        ("reserved system name", None, "file",
         "reserved or system managed"),
    ]
    reserved = (b"BPNA" + bytes([1, 3, 0, 0]) + (1).to_bytes(2, "little") +
                (14).to_bytes(2, "little") + (1).to_bytes(4, "little") +
                b"com.apple.macl" + b"v")
    for title, payload, kind, expect in cases:
        t = work / ("bad-" + title.replace(" ", "-"))
        t.mkdir()
        (t / "out").mkdir()
        (t / "payload").write_bytes(payload if payload is not None else reserved)
        r = run([helper, "t.bp", "item", "payload", kind], cwd=t)
        check(r.returncode == 0, title + ": test archive built", r.stderr)
        r = run([mac, "x", "-o", "out", "t.bp"], cwd=t)
        check(r.returncode == 1 and expect in r.stderr,
              title + ": reported, exit 1", r.stderr)
        if kind == "file":
            check((t / "out/item").read_text() == "data\n",
                  title + ": the file itself is still extracted")
        else:
            check((t / "out/item").is_dir(),
                  title + ": the directory itself is still created")

    t = work / "good-dir"
    t.mkdir(); (t / "out").mkdir()
    (t / "payload").write_bytes(good_payload(subject=4))
    run([helper, "t.bp", "item", "payload", "dir"], cwd=t)
    r = run([mac, "x", "-o", "out", "t.bp"], cwd=t)
    check(r.returncode == 0 and xattrs(t / "out/item") == {"k": b"v"},
          "a hand-built directory container is applied")

    # ---- 7. dry run, profiles, no-attribute trees ------------------------
    print("\n--- dry run, profiles, plain trees ---")
    t = work / "misc"
    (t / "src").mkdir(parents=True)
    (t / "out").mkdir()
    f = t / "src/f.txt"; f.write_text("f")
    os.setxattr(f, "user.com.example.a", b"1")
    run([mac, "c", "t.bp", "src"], cwd=t)
    r = run([mac, "x", "-n", "-o", "out", "t.bp"], cwd=t)
    check(not (t / "out/src").exists() and r.returncode == 0,
          "dry run neither writes nor fails")
    r = run([mac, "c", "--interchange-profile-1", "p.bp", "src"], cwd=t)
    check("ATTR" not in listing(mac, t / "p.bp"),
          "Interchange Profile 1 archives carry no native attribute entry")
    (t / "plain").mkdir()
    (t / "plain/p.txt").write_text("p")
    r = run([mac, "c", "plain.bp", "plain"], cwd=t)
    check("native attributes" not in r.stdout and
          "ATTR" not in listing(mac, t / "plain.bp"),
          "a tree without attributes gets no entry and no summary line")

    # ---- 8. other readers --------------------------------------------------
    print("\n--- other readers and archive edits ---")
    t = work / "rt"
    if linux.exists():
        r = run([linux, "t", "t.bp"], cwd=t)
        check(r.returncode == 0, "the Linux CLI tests the archive", r.stderr)
        (t / "lx").mkdir()
        r = run([linux, "x", "-o", "lx", "t.bp"], cwd=t)
        check((t / "lx/src/a.txt").read_text() == "hello\n" and
              "not supported on this platform" in r.stdout + r.stderr,
              "the Linux CLI extracts the data and reports the metadata")
        shutil.copy(t / "t.bp", t / "edit.bp")
        (t / "extra.txt").write_text("extra")
        r = run([linux, "a", "edit.bp", "extra.txt"], cwd=t)
        check(r.returncode == 0 and listing(mac, t / "edit.bp").count("ATTR") == 4,
              "appending with the Linux CLI keeps every attribute entry")
        r = run([linux, "d", "edit.bp", "src/a.txt"], cwd=t)
        after = listing(mac, t / "edit.bp")
        check(r.returncode == 0 and "src/a.txt" not in after and
              "ATTR  src/sub/" in after,
              "deleting a file removes its attribute entry and no other")
        shutil.copy(t / "t.bp", t / "edit2.bp")
        run([linux, "a", "edit2.bp", "extra.txt"], cwd=t)
        (t / "out3").mkdir()
        r = run([mac, "x", "-o", "out3", "edit2.bp"], cwd=t)
        check(xattrs(t / "out3/src/a.txt").get("com.example.binary") == binary,
              "attributes survive a round trip through a Linux edit")
    else:
        print("  SKIP  Linux CLI not built; cross-reader checks skipped")

    # ---- 9. extraction stays anchored (exercises the *at calls) -----------
    print("\n--- extraction anchoring, links, times and overwrite ---")
    t = work / "anchor"
    (t / "src/d1/d2").mkdir(parents=True)
    (t / "src/d1/d2/f.txt").write_text("one")
    (t / "src/g.txt").write_text("two")
    os.symlink("g.txt", t / "src/lnk")
    old = 981173106  # 2001-02-03 04:05:06 UTC
    for path in (t / "src/g.txt", t / "src/d1", t / "src/d1/d2"):
        os.utime(path, (old, old))
    os.utime(t / "src/lnk", (old + 100, old + 100), follow_symlinks=False)
    run([mac, "c", "a.bp", "src"], cwd=t)
    (t / "o1").mkdir()
    r = run([mac, "x", "-o", "o1", "a.bp"], cwd=t)
    check(r.returncode == 0, "nested tree extracts", r.stderr)
    check(os.readlink(t / "o1/src/lnk") == "g.txt", "the symlink is recreated")
    check(int((t / "o1/src/g.txt").stat().st_mtime) == old and
          int((t / "o1/src/d1").stat().st_mtime) == old and
          int((t / "o1/src/d1/d2").stat().st_mtime) == old,
          "file and directory times are restored")
    check(int(os.lstat(t / "o1/src/lnk").st_mtime) == old + 100,
          "the symlink's own time is restored, not its target's")
    (t / "elsewhere").mkdir()
    (t / "o2").mkdir()
    os.symlink("../elsewhere", t / "o2/src")
    r = run([mac, "x", "-o", "o2", "a.bp"], cwd=t)
    check(r.returncode != 0 and not list((t / "elsewhere").iterdir()),
          "extraction does not write through a symlink at the destination",
          r.stderr)
    (t / "o3/src").mkdir(parents=True)
    (t / "o3/src/g.txt").write_text("OLD")
    r = run([mac, "x", "-o", "o3", "a.bp"], cwd=t)
    check((t / "o3/src/g.txt").read_text() == "OLD" and r.returncode != 0,
          "an existing file is kept without -f")
    r = run([mac, "x", "-f", "-o", "o3", "a.bp"], cwd=t)
    check((t / "o3/src/g.txt").read_text() == "two" and r.returncode == 0,
          "-f replaces it")
    leftovers = [p.name for p in (t / "o3/src").iterdir()
                 if p.name.startswith(".boltpack")]
    check(not leftovers, "no staging files are left behind", str(leftovers))

    print("\nResults: %d passed, %d failed" % (passes, len(failures)))
    if not args.workdir or str(work) != args.workdir:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
