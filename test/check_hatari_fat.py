#!/usr/bin/env python3
"""Exercise Atari/Linux archive transfers on a disposable FAT12 floppy."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tos", type=Path, required=True)
    p.add_argument("--hatari", default="hatari")
    p.add_argument("--hconsole", type=Path,
                    default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    p.add_argument("--cc", default="m68k-atari-mint-gcc")
    p.add_argument("--atari-cli", type=Path, default=ROOT / "build/atari/boltpack.tos")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="boltpack-atari-fat-"))
    if args.output: out.mkdir(parents=True, exist_ok=False)
    host = out / "host"
    host.mkdir()
    image = out / "floppy.st"
    tos = args.tos.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    shutil.copyfile(args.atari_cli.resolve(strict=True), host / "BOLTPACK.TOS")
    data = b"Atari FAT filesystem exchange\n" * 150
    extra = b"Atari FAT append\n" * 90
    (host / "SRC.TXT").write_bytes(data)
    os.utime(host / "SRC.TXT", (951786000, 951786000))
    tree = host / "TREE"
    (tree / "NESTED").mkdir(parents=True)
    (tree / "EMPTY").mkdir()
    (tree / "NESTED" / "FILE.TXT").write_bytes(b"Nested Atari FAT file\n")
    for path in (tree / "NESTED" / "FILE.TXT", tree / "NESTED", tree / "EMPTY", tree):
        os.utime(path, (951786000, 951786000))
    (host / "NEW.TXT").write_bytes(extra)
    with (out / "commands.log").open("wb") as log:
        def run(command, cwd=None, env=None, expected=(0,), timeout=80):
            log.write((repr(command) + "\n").encode()); log.flush()
            result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            log.write(result.stdout + ("\nexit=%d\n" % result.returncode).encode()); log.flush()
            if result.returncode not in expected:
                raise RuntimeError("command failed; evidence: " + str(out))
            return result.stdout
        run([args.cc, "-O2", str(ROOT / "test/atari/guest_runner.c"), "-o", str(host / "RUN.TOS")])
        run([str(linux), "c", "--interchange-profile-1", "-1", "LINUX.BLT", "SRC.TXT"], host)
        run(["mformat", "-i", str(image), "-C", "-f", "720", "::"])
        for name in ("SRC.TXT", "NEW.TXT", "LINUX.BLT"):
            run(["mcopy", "-m", "-i", str(image), str(host / name), "::/" + name])
        run(["mcopy", "-s", "-m", "-i", str(image), str(tree), "::/TREE"])
        run(["mattrib", "-i", str(image), "+h", "+s", "::/SRC.TXT"])
        run(["mattrib", "-i", str(image), "+h", "+s", "::/TREE"])
        run(["mattrib", "-i", str(image), "+h", "+s", "::/TREE/EMPTY"])
        for name in ("OUT", "RESTORE"):
            run(["mmd", "-i", str(image), "::/" + name])
        before = out / "floppy-before.st"
        shutil.copyfile(image, before)
        commands = [
            r"x -o A:\OUT A:\LINUX.BLT",
            r"c --interchange-profile-1 A:\ATARI.BLT A:\SRC.TXT A:\TREE",
            r"a --interchange-profile-1 A:\ATARI.BLT A:\NEW.TXT",
            r"x -o A:\RESTORE A:\ATARI.BLT",
            r"d A:\ATARI.BLT NEW.TXT",
            r"t A:\ATARI.BLT",
        ]
        (host / "COMMANDS.TXT").write_text("\n".join(commands) + "\n", encoding="ascii")
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        hconsole_script = out / "HCONSOLE.TXT"
        hconsole_script.write_text("sleep 25\nquit\n", encoding="ascii")
        run([sys.executable, str(args.hconsole), str(hconsole_script), "--exit", "--",
             "--machine", "ste", "--tos", str(tos), "--sound", "off",
             "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on", "--memsize", "4",
             "--harddrive", str(host), "--disk-a", str(image), "--protect-floppy", "off",
             "--auto", r"C:\RUN.TOS"], ROOT, env, timeout=80)
        statuses = (host / "RESULT.TXT").read_text().splitlines()
        expected_statuses = ["0", "0", "0", "1", "0", "0"]
        if statuses != expected_statuses:
            raise RuntimeError("guest command statuses %r; evidence: %s" % (statuses, out))
        extract_log = (host / "LOG03.TXT").read_text(errors="replace")
        if "some attributes were not restored" not in extract_log:
            raise RuntimeError("Atari directory metadata limitation was not reported; evidence: " + str(out))
        listing = run(["mdir", "-i", str(image), "::/"])
        (out / "floppy-after-exit.txt").write_bytes(listing)
        old_bytes = before.read_bytes()
        new_bytes = image.read_bytes()
        changed = []
        for offset in range(0, min(len(old_bytes), len(new_bytes)), 512):
            old_sector = old_bytes[offset:offset + 512]
            new_sector = new_bytes[offset:offset + 512]
            if old_sector != new_sector:
                changed.append((offset // 512, sum(a != b for a, b in zip(old_sector, new_sector))))
        (out / "floppy-sector-diff.txt").write_text(
            "changed_sectors=%d\n" % len(changed) +
            "\n".join("sector=%d changed_bytes=%d" % row for row in changed) + "\n")
        if not changed:
            raise RuntimeError("Hatari did not save any floppy sectors; evidence: " + str(out))
        for src, dst in (("::/OUT/SRC.TXT", "linux-to-atari"),
                         ("::/RESTORE/SRC.TXT", "restored"),
                         ("::/RESTORE/NEW.TXT", "added"),
                         ("::/RESTORE/TREE/NESTED/FILE.TXT", "nested"),
                         ("::/ATARI.BLT", "atari.blt")):
            run(["mcopy", "-i", str(image), src, str(out / dst)])
        def fat_metadata(path, parent, short_name, is_dir=False):
            attr_text = run(["mattrib", "-p", "-i", str(image), path]).decode("ascii")
            attributes = set(re.findall(r"\+(?:r|h|s|a)", attr_text))
            listing_text = run(["mdir", "-a", "-i", str(image), parent]).decode("ascii")
            entry_line = next((line for line in listing_text.splitlines()
                               if line.split()[:1] == [short_name]), "")
            fields = entry_line.split()
            if (len(fields) < 2 or
                    (is_dir and fields[1] != "<DIR>") or
                    (not is_dir and fields[1] != "TXT")):
                raise RuntimeError("FAT entry missing for %s; evidence: %s" % (path, out))
            return attributes, tuple(fields[-2:])
        file_metadata = [
            fat_metadata("::/SRC.TXT", "::/", "SRC"),
            fat_metadata("::/RESTORE/SRC.TXT", "::/RESTORE", "SRC"),
        ]
        if not file_metadata[0][0].issubset(file_metadata[1][0]) or \
                not {"+h", "+s"}.issubset(file_metadata[1][0]):
            raise RuntimeError("Atari did not restore file hidden/system flags: %r; evidence: %s" % (file_metadata, out))
        if file_metadata[0][1] != file_metadata[1][1] or \
                not file_metadata[0][1][0].startswith("2000-"):
            raise RuntimeError("Atari FAT file timestamp changed: %r; evidence: %s" % (file_metadata, out))
        dir_source_attr = run(["mattrib", "-p", "-i", str(image), "::/TREE"]).decode("ascii")
        dir_restored_attr = run(["mattrib", "-p", "-i", str(image), "::/RESTORE/TREE"]).decode("ascii")
        (out / "restored-metadata.txt").write_text(
            "source/restored FAT file metadata=%r\n" % (file_metadata,) +
            "source TREE attributes=%r\nrestored TREE attributes=%r\n" %
            (dir_source_attr.strip(), dir_restored_attr.strip()) +
            "directory timestamp/attribute restore reports partial on TOS 1.62\n")
    if ((out / "linux-to-atari").read_bytes() != data or
            (out / "restored").read_bytes() != data or (out / "added").read_bytes() != extra):
        raise RuntimeError("FAT floppy bytes differ; evidence: " + str(out))
    if (out / "nested").read_bytes() != b"Nested Atari FAT file\n":
        raise RuntimeError("FAT directory transfer bytes differ; evidence: " + str(out))
    listing = subprocess.run([str(linux), "l", str(out / "atari.blt")], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, check=True).stdout
    (out / "linux-list.log").write_bytes(listing)
    if b"SRC.TXT" not in listing or b"TREE/" not in listing or b"NEW.TXT" in listing:
        raise RuntimeError("FAT archive delete result incorrect; evidence: " + str(out))
    extracted = out / "linux-extract"
    extracted.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(extracted), str(out / "atari.blt")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if (result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout or
            (extracted / "SRC.TXT").read_bytes() != data or
            (extracted / "TREE/nested/file.txt").read_bytes() != b"Nested Atari FAT file\n" or
            not (extracted / "TREE/empty").is_dir() or (extracted / "NEW.TXT").exists()):
        raise RuntimeError("Linux extraction of Atari archive failed; evidence: " + str(out))
    (out / "manifest.txt").write_text(
        "backend=Atari FAT12 floppy under Hatari\n" +
        "image_writeback=verified after Hatari control-console quit/eject\n" +
        "file_metadata=hidden/system flags and FAT timestamp verified\n" +
        "directory_metadata=partial on TOS 1.62; timestamp and nonzero attributes unavailable\n" +
        "rom_sha256=" + hashlib.sha256(tos.read_bytes()).hexdigest() + "\n")
    print("PASS: Atari FAT12 guest transfer and archive edits; evidence:", out)


if __name__ == "__main__":
    main()
