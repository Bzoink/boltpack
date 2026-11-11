#!/usr/bin/env python3
"""Verify the native Atari name rule on real TOS in Hatari.

A Linux-created archive holds names with Atari ST accented letters and embedded
spaces.  The Atari CLI extracts it to a FAT12 floppy under TOS, creates a new
archive from what it extracted, tests that, and extracts that again.  The floppy's
directory entries are read back from the image itself (not through mtools, which
folds accented letters), so the exact bytes TOS wrote are checked.

Runs once per TOS image given with --tos.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# UTF-8 letter -> byte in the Atari ST character set
ST = {"ü": 0x81, "é": 0x82, "ä": 0x84, "ö": 0x94, "Ä": 0x8E, "Ö": 0x99,
      "Ü": 0x9A, "ß": 0x9E, "ú": 0xA3, "ñ": 0xA4}

# (archive name, content).  Names are what a Linux user would type.
FILES = [
    ("PLAIN.TXT", b"plain ascii name\n"),
    ("café.txt", b"e acute in the base name\n"),
    ("Ärger.txt", b"A umlaut first\n"),
    ("über.dat", b"u umlaut first\n"),
    ("ñandú.c", b"two accents\n"),
    ("übung/ärger.c", b"accented directory and file\n"),
    ("src/plain.h", b"ascii directory\n"),
    ("ä.txt", b"lowercase a umlaut\n"),
    ("Ä.TXT", b"uppercase A umlaut\n"),
]


def st_bytes(name):
    """The bytes TOS should store for an archive name: ST letters, ASCII uppercased."""
    out = bytearray()
    for ch in name:
        if ch in ST:
            out.append(ST[ch])
        elif ord(ch) < 0x80:
            out += ch.upper().encode("ascii") if "a" <= ch <= "z" else ch.encode("ascii")
        else:
            raise ValueError("no ST mapping for " + ch)
    return bytes(out)


class Fat12:
    """Minimal read-only FAT12 reader that returns raw 8.3 entry bytes."""

    def __init__(self, path):
        self.d = Path(path).read_bytes()
        d = self.d
        self.bps, self.spc, self.res, self.nfat, self.rootent, self.total, _, self.spf = \
            struct.unpack_from("<HBHBHHBH", d, 11)
        self.root_start = (self.res + self.nfat * self.spf) * self.bps
        self.root_sectors = (self.rootent * 32 + self.bps - 1) // self.bps
        self.data_start = self.root_start + self.root_sectors * self.bps
        self.fat = d[self.res * self.bps:(self.res + self.spf) * self.bps]

    def next(self, cluster):
        off = cluster * 3 // 2
        v = self.fat[off] | (self.fat[off + 1] << 8)
        return (v >> 4) if cluster & 1 else (v & 0xFFF)

    def chain(self, cluster):
        out = []
        while 2 <= cluster < 0xFF0 and len(out) < 4096:
            out.append(cluster)
            cluster = self.next(cluster)
        return out

    def cluster_bytes(self, cluster):
        size = self.spc * self.bps
        off = self.data_start + (cluster - 2) * size
        return self.d[off:off + size]

    def entries(self, raw):
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if len(e) < 32 or e[0] == 0:
                return
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                continue
            name = e[0:8].rstrip(b" ")
            ext = e[8:11].rstrip(b" ")
            full = name + (b"." + ext if ext else b"")
            cluster, size = struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
            yield full, bool(e[11] & 0x10), cluster, size

    def walk(self, raw=None, prefix=b""):
        """Return {path_bytes: content or None for a directory}."""
        if raw is None:
            raw = self.d[self.root_start:self.data_start]
        tree = {}
        for name, is_dir, cluster, size in self.entries(raw):
            if name in (b".", b".."):
                continue
            path = prefix + name
            if is_dir:
                tree[path + b"/"] = None
                dirraw = b"".join(self.cluster_bytes(c) for c in self.chain(cluster))
                tree.update(self.walk(dirraw, path + b"/"))
            else:
                data = b"".join(self.cluster_bytes(c) for c in self.chain(cluster))
                tree[path] = data[:size]
        return tree


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


def run(cmd, cwd=None, env=None, timeout=120):
    return subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, timeout=timeout,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def one_tos(args, tos, work):
    label = tos.stem
    print("\n=== %s ===" % label)
    out = work / label
    shutil.rmtree(out, ignore_errors=True)
    host = out / "host"
    host.mkdir(parents=True)
    image = out / "floppy.st"
    src = out / "src"
    for name, data in FILES:
        path = src / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    # Name the top-level items so directories become explicit archive entries.
    tops = []
    for n, _ in FILES:
        top = n.split("/")[0]
        if top not in tops:
            tops.append(top)
    r = run([args.linux_cli, "c", "NAMES.BLT"] + tops, src)
    check(r.returncode == 0, "Linux creates the archive", r.stdout.decode(errors="replace"))
    shutil.copy(src / "NAMES.BLT", out / "NAMES.BLT")
    # An archive whose directories are implicit (a file path with no directory
    # entries), as Linux makes from "boltpack c a.blt dir/file".
    impl = out / "impl"
    (impl / "deep/er").mkdir(parents=True)
    (impl / "deep/er/leaf.txt").write_bytes(b"implicit parents\n")
    r = run([args.linux_cli, "c", "IMPL.BLT", "deep/er/leaf.txt"], impl)
    check(r.returncode == 0 and not any(
              " DIR " in line for line in
              run([args.linux_cli, "l", impl / "IMPL.BLT"]).stdout.decode().splitlines()),
          "the implicit-parent archive really has no directory entries")
    shutil.copyfile(args.atari_cli, host / "BOLTPACK.TOS")
    r = run([args.cc, "-O2", ROOT / "test/atari/guest_runner.c", "-o", host / "RUN.TOS"])
    check(r.returncode == 0, "guest runner builds", r.stdout.decode(errors="replace"))
    run(["mformat", "-i", image, "-C", "-f", "720", "::"])
    run(["mcopy", "-i", image, out / "NAMES.BLT", "::/NAMES.BLT"])
    run(["mcopy", "-i", image, impl / "IMPL.BLT", "::/IMPL.BLT"])
    for d in ("OUT", "OUT2", "IMPL"):
        run(["mmd", "-i", image, "::/" + d])
    commands = [
        r"x -o A:\OUT A:\NAMES.BLT",
        r"c A:\ROUND.BLT A:\OUT",
        r"t A:\ROUND.BLT",
        r"x -o A:\OUT2 A:\ROUND.BLT",
        r"x -o A:\IMPL A:\IMPL.BLT",
    ]
    (host / "COMMANDS.TXT").write_text("\n".join(commands) + "\n", encoding="ascii")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    script = out / "HCONSOLE.TXT"
    script.write_text("sleep %d\nquit\n" % args.wait, encoding="ascii")
    r = run([sys.executable, args.hconsole, script, "--exit", "--",
             "--machine", "ste", "--tos", tos, "--sound", "off",
             "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
             "--memsize", args.memsize, "--harddrive", host, "--disk-a", image,
             "--protect-floppy", "off", "--auto", r"C:\RUN.TOS"],
            ROOT, env, timeout=args.wait + 120)
    result = host / "RESULT.TXT"
    statuses = result.read_text().split() if result.exists() else []
    logs = {i: (host / ("LOG%02d.TXT" % i)).read_text(errors="replace")
            if (host / ("LOG%02d.TXT" % i)).exists() else "" for i in range(5)}
    for i, text in logs.items():
        (out / ("log%d.txt" % i)).write_text(text)
    tree = Fat12(image).walk()
    (out / "floppy-tree.txt").write_text("\n".join(
        "%r %s" % (k, "dir" if v is None else len(v)) for k, v in sorted(tree.items())))
    # TOS cannot set directory times, so extracting directories reports partial
    # (status 1).  Anything else the tool prints is a failure.
    allowed = ("cannot restore directory timestamp", "some attributes were not restored")
    stray = [line for text in logs.values() for line in text.splitlines()
             if line.startswith("boltpack:") and not any(a in line for a in allowed)]
    check(statuses == ["1", "0", "0", "1", "0"] and not stray,
          "extract, create, test and re-extract succeed on TOS (directory times are the only partial)",
          "statuses=%r stray=%r" % (statuses, stray))
    if len(statuses) != 5:
        return
    check(tree.get(b"IMPL/DEEP/ER/LEAF.TXT") == b"implicit parents\n",
          "a file whose parent directories are implicit extracts on TOS")

    def expected(prefix):
        want = {}
        for name, data in FILES:
            parts = name.split("/")
            raw = prefix + b"/".join(st_bytes(p) for p in parts)
            for k in range(1, len(parts)):
                want[prefix + b"/".join(st_bytes(p) for p in parts[:k]) + b"/"] = None
            want[raw] = data
        return want

    # OUT holds the extracted Linux archive; OUT2 holds ROUND.BLT, which was made
    # from OUT and so carries an OUT/ prefix of its own.
    for dirname in (b"OUT/", b"OUT2/OUT/"):
        want = expected(dirname)
        got = {k: v for k, v in tree.items() if k.startswith(dirname)}
        # the two ä/Ä names are checked separately below
        fold_a, fold_b = (dirname + st_bytes("ä.txt"), dirname + st_bytes("Ä.TXT"))
        for key, data in sorted(want.items()):
            if key in (fold_a, fold_b):
                continue
            check(key in got and (data is None or got[key] == data),
                  "%s%s exists with its exact bytes" % (dirname.decode(), key[len(dirname):].decode("latin-1")),
                  "missing or different")
        both = fold_a in got and fold_b in got and got[fold_a] != got[fold_b]
        check(both or (fold_a in got) != (fold_b in got),
              "%s lowercase and uppercase umlaut names: kept apart or one refused, never merged silently"
              % dirname.decode())
        print("        observed: %s" % ("both kept distinct" if both else
                                       "only one present (TOS folds accented letters)"))

    listing = run([args.linux_cli, "l", out / "NAMES.BLT"]).stdout.decode()
    check(all(name.rstrip("/").split("/")[-1] in listing for name, _ in FILES),
          "the Linux archive lists every UTF-8 name")
    round_trip = run(["mcopy", "-i", image, "::/ROUND.BLT", out / "ROUND.BLT"])
    names = run([args.linux_cli, "l", out / "ROUND.BLT"]).stdout.decode(errors="replace")
    # TOS-side create lowercases ASCII letters (the C library does); the accented
    # letters come back as the UTF-8 for the ST bytes that were on the disk.
    wanted = ("OUT/café.txt", "OUT/Ärger.txt", "OUT/über.dat", "OUT/ñandú.c",
              "OUT/übung/ärger.c", "OUT/src/plain.h", "OUT/ä.txt", "OUT/Ä.txt")
    missing = [n for n in wanted if n not in names]
    check(not missing,
          "an archive made on TOS from those names lists them as UTF-8",
          "missing %r in\n%s" % (missing, names))
    return tree


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tos", type=Path, action="append",
                    help="TOS ROM image (repeatable)")
    ap.add_argument("--hconsole", default="/usr/share/hatari/hconsole/hconsole.py")
    ap.add_argument("--cc", default="m68k-atari-mint-gcc")
    ap.add_argument("--memsize", default="4")
    ap.add_argument("--wait", type=int, default=30)
    ap.add_argument("--atari-cli", type=Path, default=ROOT / "build/atari/boltpack.tos")
    ap.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    ap.add_argument("--keep", type=Path, help="keep evidence in this directory")
    args = ap.parse_args()
    if not args.tos:
        print("SKIP: give at least one --tos ROM image")
        return 0
    args.atari_cli = args.atari_cli.resolve(strict=True)
    args.linux_cli = args.linux_cli.resolve(strict=True)
    if args.keep:
        args.keep = args.keep.resolve()
    work = args.keep or Path(tempfile.mkdtemp(prefix="atari-names-"))
    work.mkdir(parents=True, exist_ok=True)
    for tos in args.tos:
        one_tos(args, tos.expanduser().resolve(strict=True), work)
    print("\nResults: %d passed, %d failed" % (passes, len(failures)))
    print("evidence: %s" % work)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
