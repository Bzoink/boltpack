#!/usr/bin/env python3
"""Integration tests for multi-volume archives (archive format 7).

Usage:
    python3 test/check_volumes.py [--boltpack PATH] [--flavor linux|dos]
                                  [--runner COMMAND] [--codecs LIST]
                                  [--files]
                                  [--verbose]

--flavor dos runs the same checks against the DOS CLI (for example the host
build from `make dos-host-test`), which has no -2 option (BoltLZ is its
default) and stores no symbolic links.  --runner prefixes every command (for
example `wine` to test the Windows CLI).  --codecs lists the codec options
the CLI has (default -2,-F,-1,-0), for frontends such as the X68000 CLI that
offer only some of them.  --files names the source files one by one instead of
passing the `src` directory, for CLIs that cannot walk a directory on the host.

Covers creating volume sets with -v, reading them from any volume, naming,
volume sizes, stale-volume clean-up, missing/foreign/truncated volumes,
add/delete refusal on sets, and --pause media changes driven through the
prompts (each "disk" is a directory; swapping disks moves files in and out
of a "drive" directory).
"""

import argparse
import filecmp
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILS = []
VERBOSE = False
FLAVOR = "linux"
RUNNER = []
FILES = False
CODECS = ("-2", "-F", "-1", "-0")


def check(cond, what):
    if cond:
        if VERBOSE:
            print("  PASS  " + what)
    else:
        print("  FAIL  " + what)
        FAILS.append(what)


def run(bp, args, cwd, stdin=None):
    p = subprocess.run(RUNNER + [bp] + args, cwd=cwd, input=stdin,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=600)
    return p.returncode, p.stdout.decode("utf-8", "replace"), \
        p.stderr.decode("utf-8", "replace")


def make_tree(base):
    rnd = random.Random(7)
    src = os.path.join(base, "src")
    os.makedirs(os.path.join(src, "sub", "deeper"))
    words = b"volume set diskette boltpack archive compress the quick brown fox "
    with open(os.path.join(src, "text.txt"), "wb") as f:
        for _ in range(60000):
            f.write(rnd.choice(words.split()) + b" ")
    with open(os.path.join(src, "sub", "random.bin"), "wb") as f:
        f.write(bytes(rnd.getrandbits(8) for _ in range(1500000)))
    with open(os.path.join(src, "sub", "deeper", "mixed.dat"), "wb") as f:
        for i in range(40):
            if i % 2:
                f.write(bytes(rnd.getrandbits(8) for _ in range(20000)))
            else:
                f.write(words * 300)
    with open(os.path.join(src, "sub", "empty"), "wb"):
        pass
    for i in range(30):
        with open(os.path.join(src, "sub", "small%02d.txt" % i), "wb") as f:
            f.write(b"small file %d\n" % i * (i + 1))
    if FLAVOR != "dos":
        os.symlink("text.txt", os.path.join(src, "link"))
    return src


def same_tree(a, b):
    cmp = filecmp.dircmp(a, b)
    if cmp.left_only or cmp.right_only or cmp.diff_files or cmp.funny_files:
        return False
    for f in cmp.common_files:
        pa, pb = os.path.join(a, f), os.path.join(b, f)
        if os.path.islink(pa) != os.path.islink(pb):
            return False
        if os.path.islink(pa):
            if os.readlink(pa) != os.readlink(pb):
                return False
        elif not filecmp.cmp(pa, pb, shallow=False):
            return False
    return all(same_tree(os.path.join(a, d), os.path.join(b, d))
               for d in cmp.common_dirs)


def sources(d):
    """Arguments naming the tree to archive in directory d."""
    if not FILES:
        return ["src"]
    names = []
    for root, dirs, files in os.walk(os.path.join(d, "src")):
        dirs.sort()
        for f in sorted(files):
            names.append(os.path.relpath(os.path.join(root, f), d))
    return names


def boltlz():
    """Option list selecting BoltLZ."""
    return [] if FLAVOR == "dos" else ["-2"]


def volumes(d, stem="backup"):
    return sorted(n for n in os.listdir(d)
                  if n.startswith(stem + ".") and n != stem + ".tmp")


def test_basic(bp, work, src):
    print("create, read from any volume, extract")
    for codec, size in (("-2", "200K"), ("-F", "300K"), ("-1", "250000"),
                        ("-0", "1.44M"), ("-F", "64Ki")):
        if codec not in CODECS:
            continue
        d = tempfile.mkdtemp(dir=work)
        shutil.copytree(src, os.path.join(d, "src"), symlinks=True)
        opts = boltlz() if codec == "-2" else [codec]
        rc, out, err = run(bp, ["c"] + opts + ["-v", size, "backup.bolt"] + sources(d), d)
        check(rc == 0, "create %s -v %s (%s)" % (codec, size, err.strip()[-200:]))
        names = volumes(d)
        limit = {"200K": 200000, "300K": 300000, "250000": 250000,
                 "1.44M": 1440000, "64Ki": 65536}[size]
        check(names[-1] == "backup.bolt" and
              all(n == "backup.%03d" % i for i, n in enumerate(names[:-1])),
              "%s: volume names %s" % (size, names[:3] + ["..."] + names[-1:]))
        check(all(os.path.getsize(os.path.join(d, n)) <= limit for n in names),
              "%s: every volume at most %d bytes" % (size, limit))
        check(len(names) > 1, "%s: several volumes (%d)" % (size, len(names)))
        sizes = [os.path.getsize(os.path.join(d, n)) for n in names[:-2]]
        check(all(s >= limit - 600 for s in sizes),
              "%s: volumes before the table are full (%s)" % (size, sizes[:4]))
        check("volumes" in out, "%s: summary names the volumes" % size)
        for given in (names[0], names[len(names) // 2], "backup.bolt"):
            rc, out, err = run(bp, ["l", given], d)
            check(rc == 0 and "src/sub/random.bin" in out,
                  "%s: list via %s" % (size, given))
        rc, out, err = run(bp, ["t", names[1]], d)
        check(rc == 0, "%s: test via %s" % (size, names[1]))
        rc, out, err = run(bp, ["i", "backup.bolt"], d)
        check(rc == 0 and re.search(r"Volumes:\s+%d\b" % len(names), out) and
              "(all volumes)" in out, "%s: info shows the volumes" % size)
        rc, out, err = run(bp, ["x", "-q", "-o", "out", names[0]], d)
        check(rc == 0 and same_tree(os.path.join(d, "src"),
                                    os.path.join(d, "out", "src")),
              "%s: extracted tree matches" % size)
        shutil.rmtree(d)


def test_single_and_names(bp, work, src):
    print("single volume sets, .blt names, stale volumes")
    d = tempfile.mkdtemp(dir=work)
    shutil.copytree(src, os.path.join(d, "src"), symlinks=True)
    rc, out, err = run(bp, ["c", "-v", "700M", "backup.bolt"] + sources(d), d)
    check(rc == 0 and volumes(d) == ["backup.bolt"],
          "a set that fits one volume is just backup.bolt")
    rc, out, err = run(bp, ["c", "-v", "200K", "data.blt"] + sources(d), d)
    names = volumes(d, "data")
    check(rc == 0 and names[-1] == "data.blt" and names[0] == "data.000",
          "a .blt archive name keeps .blt for the last volume")
    rc, out, err = run(bp, ["x", "-q", "-o", "o1", "data.003"], d)
    check(rc == 0 and same_tree(os.path.join(d, "src"),
                                os.path.join(d, "o1", "src")),
          "extract via a numbered volume finds data.blt")
    before = len(volumes(d, "data"))
    rc, out, err = run(bp, ["c", "-v", "600K", "data.blt"] + sources(d), d)
    after = volumes(d, "data")
    check(rc == 0 and len(after) < before and "left over" in err,
          "a smaller set removes stale numbered volumes (%d -> %d)"
          % (before, len(after)))
    rc, out, err = run(bp, ["t", "data.blt"], d)
    check(rc == 0, "the smaller set tests clean")
    with open(os.path.join(d, "notes.005"), "wb") as f:
        f.write(b"not a volume")
    rc, out, err = run(bp, ["c", "-v", "200K", "notes.005"] + sources(d), d)
    check(rc != 0 and "cannot name a volume set" in err,
          "an archive name with a three-digit extension is refused")
    shutil.rmtree(d)


def test_errors(bp, work, src):
    print("missing, foreign and truncated volumes; editing sets")
    d = tempfile.mkdtemp(dir=work)
    shutil.copytree(src, os.path.join(d, "src"), symlinks=True)
    run(bp, ["c"] + boltlz() + ["-v", "200K", "backup.bolt"] + sources(d), d)
    run(bp, ["c"] + boltlz() + ["-v", "200K", "other.bolt"] + sources(d), d)
    os.rename(os.path.join(d, "backup.002"), os.path.join(d, "away"))
    rc, out, err = run(bp, ["x", "-o", "out", "backup.bolt"], d, stdin=b"")
    check(rc != 0 and "backup.002" in err and "nothing was extracted" in err and
          not os.path.exists(os.path.join(d, "out")),
          "a missing volume is named and nothing is written")
    rc, out, err = run(bp, ["t", "backup.bolt"], d, stdin=b"")
    check(rc != 0 and "backup.002" in err, "test names the missing volume")
    shutil.copy(os.path.join(d, "other.002"), os.path.join(d, "backup.002"))
    rc, out, err = run(bp, ["t", "backup.bolt"], d, stdin=b"")
    check(rc != 0 and "another archive" in err,
          "a volume from another set is rejected")
    os.replace(os.path.join(d, "away"), os.path.join(d, "backup.002"))
    with open(os.path.join(d, "backup.001"), "r+b") as f:
        f.truncate(os.path.getsize(os.path.join(d, "backup.001")) - 50)
    rc, out, err = run(bp, ["t", "backup.bolt"], d, stdin=b"")
    check(rc != 0 and "backup.001" in err and "truncated" in err,
          "a truncated volume is named")
    rc, out, err = run(bp, ["a", "backup.bolt", "src/text.txt"], d)
    check(rc != 0 and "multi-volume" in err, "add refuses a volume set")
    rc, out, err = run(bp, ["d", "other.bolt", "src/text.txt"], d)
    check(rc != 0 and "multi-volume" in err, "delete refuses a volume set")
    rc, out, err = run(bp, ["c", "-v", "15K", "x.bolt"] + sources(d), d)
    check(rc != 0 and "16K" in err, "volumes below 16K are refused")
    rc, out, err = run(bp, ["c", "-v", "1.44X", "x.bolt"] + sources(d), d)
    check(rc != 0 and "invalid volume size" in err, "bad sizes are refused")
    rc, out, err = run(bp, ["x", "-v", "1M", "other.bolt"], d)
    check(rc != 0 and "-v applies to create" in err, "-v only with create")
    with open(os.path.join(d, "old.bolt"), "wb") as f:
        f.write(b"BOLT\x06\x00" + bytes(100))
    rc, out, err = run(bp, ["l", "old.bolt"], d)
    check(rc != 0 and "format 7" in err, "format 6 archives are reported")
    shutil.rmtree(d)


class Session:
    """Drive one boltpack run that prompts for media on stderr."""

    def __init__(self, bp, args, cwd):
        self.p = subprocess.Popen(RUNNER + [bp] + args, cwd=cwd, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.err = b""

    def wait_prompt(self):
        """Read stderr up to the next prompt; None when the run ended."""
        marker = b"type q to quit: "
        while not self.err.endswith(marker):
            c = self.p.stderr.read(1)
            if not c:
                return None
            self.err += c
        text = self.err.decode("utf-8", "replace")
        self.err = b""
        return text

    def answer(self, line):
        self.p.stdin.write(line)
        self.p.stdin.flush()

    def finish(self):
        out, err = self.p.communicate(timeout=600)
        return self.p.returncode, (self.err + err).decode("utf-8", "replace")


def test_pause(bp, work, src):
    print("--pause: writing and reading with media changes")
    d = tempfile.mkdtemp(dir=work)
    shutil.copytree(src, os.path.join(d, "src"), symlinks=True)
    drive = os.path.join(d, "drive")
    os.mkdir(drive)
    disks = []

    s = Session(bp, ["c"] + boltlz() + ["-v", "300K", "--pause",
                                       "drive/backup.bolt"] + sources(d), d)
    prompts = 0
    while True:
        text = s.wait_prompt()
        if text is None:
            break
        prompts += 1
        # eject the previous disk: move what is in the drive away
        if os.listdir(drive):
            disk = os.path.join(d, "disk%d" % len(disks))
            shutil.move(drive, disk)
            os.mkdir(drive)
            disks.append(disk)
        s.answer(b"\n")
    rc, err = s.finish()
    disk = os.path.join(d, "disk%d" % len(disks))
    shutil.move(drive, disk)
    os.mkdir(drive)
    disks.append(disk)
    contents = [os.listdir(x) for x in disks]
    check(rc == 0, "create with --pause (%s)" % err.strip()[-200:])
    check(prompts == len(disks) and all(len(c) == 1 for c in contents),
          "one prompt and one volume per disk (%d prompts, %s)"
          % (prompts, contents))
    check(contents[-1] == ["backup.bolt"] and contents[0] == ["backup.000"],
          "first disk holds backup.000, last disk backup.bolt")

    # reading: start with the last disk in the drive
    def insert(i):
        for n in os.listdir(drive):
            os.remove(os.path.join(drive, n))
        for n in os.listdir(disks[i]):
            shutil.copy(os.path.join(disks[i], n), os.path.join(drive, n))

    insert(len(disks) - 1)
    s = Session(bp, ["x", "-q", "--pause", "-o", "out", "drive/backup.bolt"], d)
    asked = []
    while True:
        text = s.wait_prompt()
        if text is None:
            break
        want = None
        for i, x in enumerate(disks):
            name = os.listdir(x)[0]
            if name in text.split("\n")[-2]:
                want = i
        asked.append(want)
        if want is None:
            s.answer(b"q\n")
            break
        insert(want)
        s.answer(b"\n")
    rc, err = s.finish()
    check(rc == 0 and same_tree(os.path.join(d, "src"),
                                os.path.join(d, "out", "src")),
          "extract with media changes (%s)" % err.strip()[-200:])
    expected = list(range(len(disks)))
    check(asked == expected[:-1] or asked == expected,
          "disks requested in order once (%s)" % asked)

    # a wrong disk is rejected and asked for again
    insert(len(disks) - 1)
    s = Session(bp, ["t", "--pause", "drive/backup.bolt"], d)
    first = s.wait_prompt()
    # a mislabeled disk: the expected file name, but volume 2's contents
    insert(1)
    os.rename(os.path.join(drive, "backup.001"),
              os.path.join(drive, "backup.000"))
    s.answer(b"\n")
    second = s.wait_prompt()
    check(second is not None and "wrong volume" in second,
          "a wrong disk is refused and asked for again")
    s.answer(b"q\n")
    rc, err = s.finish()
    check(rc != 0 and "stopped at your request" in err, "q stops the run")
    shutil.rmtree(d)


def main():
    global VERBOSE, FLAVOR, RUNNER, CODECS, FILES
    ap = argparse.ArgumentParser()
    ap.add_argument("--boltpack",
                    default=os.path.join(ROOT, "build", "linux", "boltpack"))
    ap.add_argument("--flavor", choices=("linux", "dos"), default="linux")
    ap.add_argument("--runner", default="")
    ap.add_argument("--codecs", default="-2,-F,-1,-0")
    ap.add_argument("--files", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    RUNNER = a.runner.split()
    CODECS = tuple(a.codecs.split(","))
    FILES = a.files
    VERBOSE = a.verbose
    FLAVOR = a.flavor
    bp = os.path.abspath(a.boltpack)
    work = tempfile.mkdtemp(prefix="bpvol_")
    try:
        src = make_tree(work)
        test_basic(bp, work, src)
        test_single_and_names(bp, work, src)
        test_errors(bp, work, src)
        test_pause(bp, work, src)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if FAILS:
        print("%d check(s) failed" % len(FAILS))
        return 1
    print("all volume checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
