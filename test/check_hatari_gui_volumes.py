#!/usr/bin/env python3
"""Drive the Atari GEM GUI through disk-size sets in Hatari.

Create: Ctrl+D steps the Compression menu's Disks row to 360K, the GUI archives
an incompressible file, and the sizes of the volumes it wrote are checked
against the disk size and the set is extracted by the Linux CLI.

Open and extract: a set made by the Linux CLI is opened and extracted by the
GUI, which must also report "Archive info, 3 disks".

Every volume stays in one folder on a GEMDOS hard drive, so no disk is
swapped: the disk prompt appears (the create run answers it with Return) but
the prompt logic, wrong disks and Cancel are covered by
test/unit/test_gui_volumes.c (on a host).  No emulator test swaps a real
diskette image in the GUI or in the CLI's --pause.
"""
import argparse
import hashlib
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
DISK_LIMIT = 350000      # the 360K entry in atari_disks[]


def run(command, cwd, env=None, timeout=240):
    return subprocess.run(command, cwd=cwd, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)


def hatari(args, script_lines, host, out, name):
    script = out / (name + ".TXT")
    script.write_text("\n".join(script_lines + ["quit", ""]), encoding="ascii")
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    command = [
        sys.executable, str(args.hconsole), str(script), "--exit", "--",
        "--machine", "ste", "--tos", str(args.tos), "--sound", "off",
        "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
        "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG",
    ]
    result = run(command, out, env=env, timeout=args.timeout)
    (out / (name + ".log")).write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError("Hatari run %s failed; evidence: %s" % (name, out))


def ctrl(key):
    return ["keydown 29", "keypress " + key, "keyup 29"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", type=Path,
                        default=ROOT / "build/atari/boltpack.prg")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    parser.add_argument("--size", type=int, default=800000,
                        help="bytes of incompressible test data")
    parser.add_argument("--timeout", type=int, default=420)
    args = parser.parse_args()
    args.tos = args.tos.expanduser().resolve(strict=True)
    args.hconsole = args.hconsole.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    args.output_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-gui-volumes-", dir=args.output_root))

    data = random.Random(7).randbytes(args.size)
    digest = hashlib.sha256(data).hexdigest()

    # ---- create ----------------------------------------------------------
    host = out / "create"
    host.mkdir()
    shutil.copy2(gui, host / "BOLTPACK.PRG")
    (host / "DATA.BIN").write_bytes(data)
    hatari(args, ["sleep 20"] + ctrl("d") + [
        "sleep 2",
    ] + ctrl("n") + [
        "sleep 4", "text DATA.BIN", "keypress 28", "sleep 3",
        "keypress 28",                       # Create (the alert's default)
        "sleep 4", "keypress 28",            # default ARCHIVE.BLT
        "sleep 150",
        # Disk prompts for volumes 2 and 3, then the "Written as" alert.
        "screenshot", "keypress 28", "sleep 60", "keypress 28", "sleep 60",
        "keypress 28", "sleep 10", "screenshot", "keypress 28", "sleep 2",
    ], host, out, "create")
    names = sorted(p.name for p in host.iterdir()
                   if p.name.upper().startswith("ARCHIVE"))
    print("created:", names)
    volumes = [host / n for n in names]
    if len(volumes) < 3:
        raise RuntimeError("expected a set of 3 volumes, got %s; evidence: %s"
                           % (names, out))
    for volume in volumes:
        size = volume.stat().st_size
        if size > DISK_LIMIT:
            raise RuntimeError("%s is %d bytes, over the %d limit; evidence: %s"
                               % (volume.name, size, DISK_LIMIT, out))
    if any(p.name.startswith(("BPVOL", "BP")) and p.suffix == ".TMP" or
           p.name.upper().startswith("BPX") for p in host.iterdir()):
        raise RuntimeError("temporary volume left behind; evidence: " + str(out))
    last = host / "ARCHIVE.BLT"
    extracted = out / "linux-out"
    extracted.mkdir()
    result = run([str(linux), "x", "-o", str(extracted), str(last)], out)
    (out / "linux-extract.log").write_text(result.stdout, encoding="utf-8")
    got = extracted / "DATA.BIN"
    # The CLI exits 1 for Atari metadata it cannot restore on Linux; the data
    # must still be intact.
    if not got.exists() or \
            hashlib.sha256(got.read_bytes()).hexdigest() != digest:
        raise RuntimeError("Linux CLI did not restore the GUI's set; evidence: "
                           + str(out))

    # ---- open and extract a set made by the Linux CLI -------------------
    host2 = out / "extract"
    host2.mkdir()
    shutil.copy2(gui, host2 / "BOLTPACK.PRG")
    (host2 / "OUT").mkdir()
    made = out / "cli-src"
    made.mkdir()
    (made / "DATA.BIN").write_bytes(data)
    result = run([str(linux), "c", "-v", str(DISK_LIMIT), "-b", "16k",
                  str(host2 / "SET.BLT"), "DATA.BIN"], made)
    (out / "linux-create.log").write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0 or not (host2 / "SET.000").exists():
        raise RuntimeError("Linux CLI could not make the set; evidence: " + str(out))
    hatari(args, ["sleep 20"] + ctrl("o") + [
        "sleep 3", "text SET.BLT", "keypress 28", "sleep 5", "screenshot",
    ] + ctrl("e") + [
        "sleep 3", "text OUT", "keypress 28", "sleep 90", "screenshot",
        "keypress 28", "sleep 2",
    ], host2, out, "extract")
    got = host2 / "OUT" / "DATA.BIN"
    if not got.exists() or hashlib.sha256(got.read_bytes()).hexdigest() != digest:
        raise RuntimeError("GUI did not restore the CLI's set; evidence: " + str(out))
    print("PASS: Atari GUI disk-size sets; evidence:", out)


if __name__ == "__main__":
    main()
