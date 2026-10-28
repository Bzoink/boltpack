#!/usr/bin/env python3
"""Exercise Atari/Linux archive transfers on a disposable FAT12 floppy."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tos", type=Path, required=True)
    p.add_argument("--hatari", default="hatari")
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
            run(["mcopy", "-i", str(image), str(host / name), "::/" + name])
        for name in ("OUT", "RESTORE"):
            run(["mmd", "-i", str(image), "::/" + name])
        commands = [
            r"x -o A:\OUT A:\LINUX.BLT",
            r"c --interchange-profile-1 A:\ATARI.BLT A:\SRC.TXT",
            r"a --interchange-profile-1 A:\ATARI.BLT A:\NEW.TXT",
            r"x -o A:\RESTORE A:\ATARI.BLT",
            r"d A:\ATARI.BLT NEW.TXT",
            r"t A:\ATARI.BLT",
        ]
        (host / "COMMANDS.TXT").write_text("\n".join(commands) + "\n", encoding="ascii")
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        run([args.hatari, "--machine", "ste", "--tos", str(tos), "--sound", "off",
             "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on", "--memsize", "4",
             "--harddrive", str(host), "--disk-a", str(image), "--protect-floppy", "off",
             "--auto", r"C:\RUN.TOS", "--run-vbls", "18000"], ROOT, env)
        statuses = (host / "RESULT.TXT").read_text().splitlines()
        if statuses != ["0"] * len(commands):
            raise RuntimeError("guest command statuses %r; evidence: %s" % (statuses, out))
        listing = run(["mdir", "-i", str(image), "::/"])
        (out / "floppy-after-exit.txt").write_bytes(listing)
        for src, dst in (("CAPSRC.TXT", "linux-to-atari"),
                         ("CAPREST.TXT", "restored"),
                         ("CAPNEW.TXT", "added"), ("CAPATARI.BLT", "atari.blt")):
            shutil.copyfile(host / src, out / dst)
    if ((out / "linux-to-atari").read_bytes() != data or
            (out / "restored").read_bytes() != data or (out / "added").read_bytes() != extra):
        raise RuntimeError("FAT floppy bytes differ; evidence: " + str(out))
    listing = subprocess.run([str(linux), "l", str(out / "atari.blt")], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, check=True).stdout
    (out / "linux-list.log").write_bytes(listing)
    if b"SRC.TXT" not in listing or b"NEW.TXT" in listing:
        raise RuntimeError("FAT archive delete result incorrect; evidence: " + str(out))
    extracted = out / "linux-extract"
    extracted.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(extracted), str(out / "atari.blt")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if (result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout or
            (extracted / "SRC.TXT").read_bytes() != data or (extracted / "NEW.TXT").exists()):
        raise RuntimeError("Linux extraction of Atari archive failed; evidence: " + str(out))
    (out / "manifest.txt").write_text("backend=Atari FAT12 floppy under Hatari\nimage_writeback=directory changes not retained after emulator exit\nrom_sha256=" + hashlib.sha256(tos.read_bytes()).hexdigest() + "\n")
    print("PASS: Atari FAT12 guest transfer and archive edits; evidence:", out)


if __name__ == "__main__":
    main()
