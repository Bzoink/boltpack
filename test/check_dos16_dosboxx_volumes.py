#!/usr/bin/env python3
"""Volume sets (archive format 7) with the 16-bit DOS mini CLI on DOSBox-X.

Writes a set onto a FAT12 1.44 MB floppy image with --pause (answering the
prompts from a file), reads it back from any volume, replaces it with a
smaller set, checks the refusal when a volume is missing, and exchanges sets
with the Linux CLI in both directions.  Files of 32 KiB and more exercise
full 32 KiB blocks, which 16-bit read() and write() report as negative
counts.

Usage:
    python3 test/check_dos16_dosboxx_volumes.py [--dosbox-x PATH]
"""
import argparse
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FAILS = []


def check(cond, what):
    print(("  PASS  " if cond else "  FAIL  ") + what)
    if not cond:
        FAILS.append(what)


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def mdir(image):
    out = run(["mdir", "-i", str(image), "-b", "::/"]).decode("ascii", "replace")
    return sorted(line.strip().split("/")[-1].upper()
                  for line in out.splitlines() if line.strip())


def dosbox(args, cdir, image, commands, log, limit=600):
    """Run commands in one short DOSBox-X session, which DOSBox-X ends
    after `limit` seconds; a marker shows that every command returned."""
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    marker = cdir / "DONE.TXT"
    if marker.exists():
        marker.unlink()
    cmd = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch",
           "-time-limit", str(limit), "-set", "dos lfn=false",
           "-set", "cpu cycles=max",
           "-c", "mount c " + str(cdir),
           "-c", "imgmount a " + str(image) + " -t floppy", "-c", "c:",
           "-c", "set BP_TZ_OFFSET=0"]
    for c in commands:
        cmd.extend(("-c", c))
    cmd.extend(("-c", "echo done > DONE.TXT", "-c", "exit"))
    log.write_bytes(subprocess.run(cmd, env=env, timeout=limit + 60,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT).stdout)
    check(marker.exists(), "DOSBox-X session %s returned from every command"
          % log.name)


def same_tree(a, b):
    """Same files and bytes, comparing names case-insensitively (FAT)."""
    def files(root):
        # DOSBox-X keeps DOS attributes of mounted host directories in
        # .DBLOCALFILE_ATR_* files; they are not archive contents.
        return {str(p.relative_to(root)).upper(): p.read_bytes()
                for p in root.rglob("*")
                if p.is_file() and not p.name.startswith(".DBLOCALFILE")}
    return b.is_dir() and files(a) == files(b)


def text(path):
    return path.read_text(errors="replace") if path.exists() else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dosbox-x", default="dosbox-x")
    ap.add_argument("--output", type=Path,
                    help="new directory for images and logs (kept)")
    args = ap.parse_args()
    exe = ROOT / "build/dos16/bpmini.exe"
    linux = ROOT / "build/linux/boltpack"
    if not exe.is_file() or not linux.is_file():
        raise SystemExit("build first: make modern dos16")
    out = args.output.resolve() if args.output else Path(
        tempfile.mkdtemp(prefix="boltpack-dos16-volumes-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)

    c = out / "c"
    (c / "DOCS" / "SUB").mkdir(parents=True)
    rnd = random.Random(1440)
    (c / "DOCS" / "RANDOM.BIN").write_bytes(
        bytes(rnd.getrandbits(8) for _ in range(800000)))
    words = b"diskette volume set for DOS archive restore "
    (c / "DOCS" / "SUB" / "TEXT.TXT").write_bytes(words * 6000)
    (c / "DOCS" / "SMALL.TXT").write_bytes(b"small file\r\n")
    shutil.copyfile(exe, c / "BPMINI.EXE")
    (c / "KEYS.TXT").write_bytes(b"\r\n" * 20)
    (c / "EMPTY.TXT").write_bytes(b"")

    # a set made by Linux, readable by mini (BoltLZ, 32 KiB blocks)
    run([str(linux), "c", "-2", "-b", "32k", "-v", "150K", "LX.BLT", "DOCS"],
        cwd=c)
    linux_vols = sorted(p.name for p in c.glob("LX.*"))

    floppy = out / "floppy.img"
    run(["mformat", "-i", str(floppy), "-C", "-f", "1440", "::"])

    dosbox(args, c, floppy, [
        "bpmini c -v 360K --pause A:\\BACKUP.BLT DOCS < KEYS.TXT > W1.TXT",
        "bpmini l A:\\BACKUP.000 > L1.TXT",
        "bpmini i A:\\BACKUP.BLT > I1.TXT",
        "bpmini t A:\\BACKUP.001 > T1.TXT",
    ], out / "dosbox-write.log")
    dosbox(args, c, floppy, [
        "bpmini x A:\\BACKUP.000 OUT1 < EMPTY.TXT > X1.TXT",
        "bpmini x LX.001 OUTLX < EMPTY.TXT > XLX.TXT",
        "bpmini c -v 100K SETC.BLT DOCS > W2.TXT",
    ], out / "dosbox-read.log")

    after_first = mdir(floppy)
    check(after_first == ["BACKUP.000", "BACKUP.001", "BACKUP.BLT"],
          "the set is written as BACKUP.000, BACKUP.001 and BACKUP.BLT (%s)"
          % after_first)
    check("3 volumes" in text(c / "W1.TXT"),
          "create with --pause (answers from standard input) reports 3 volumes")
    check("DOCS/RANDOM.BIN" in text(c / "L1.TXT").upper(),
          "list through BACKUP.000 reads the table from BACKUP.BLT")
    info = text(c / "I1.TXT")
    check("Volumes:     3" in info and "Version:     7" in info,
          "info shows format 7 and 3 volumes")
    check("FAIL" not in text(c / "T1.TXT") and "OK" in text(c / "T1.TXT"),
          "test through BACKUP.001 passes")
    check(same_tree(c / "DOCS", c / "OUT1" / "DOCS"),
          "extract through BACKUP.000 restores the tree")
    check(same_tree(c / "DOCS", c / "OUTLX" / "DOCS"),
          "dos16 extracts the Linux-made set (%d volumes)" % len(linux_vols))

    # Linux reads the dos16-made sets
    vols = out / "from-floppy"
    vols.mkdir()
    for name in after_first:
        run(["mcopy", "-i", str(floppy), "::/" + name, str(vols / name)])
    lx_out = out / "linux-out"
    rc = subprocess.run([str(linux), "x", "-q", "-o", str(lx_out),
                         str(vols / "BACKUP.BLT")], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
    check(rc.returncode in (0, 1) and same_tree(c / "DOCS", lx_out / "DOCS"),
          "Linux extracts the set written on the floppy")
    setc = sorted(p.name for p in c.glob("SETC.*"))
    rc = subprocess.run([str(linux), "t", str(c / "SETC.BLT")],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check(rc.returncode == 0 and len(setc) >= 4,
          "Linux tests the dos16 set written to C: (%d volumes)" % len(setc))

    # replace with a smaller set; take a volume away
    (c / "SETC.002").rename(c / "AWAY.002")
    dosbox(args, c, floppy, [
        "bpmini c -v 1.44M A:\\BACKUP.BLT DOCS\\SUB > W3.TXT",
        "bpmini t A:\\BACKUP.BLT > T3.TXT",
        "bpmini x SETC.BLT OUT2 < EMPTY.TXT > X2.TXT",
        "if errorlevel 1 echo FAIL > X2.BAD",
    ], out / "dosbox-replace.log")
    after_second = mdir(floppy)
    check(after_second == ["BACKUP.BLT"],
          "a one-volume set replaces the three-volume set and its leftover "
          "volumes are removed (%s)" % after_second)
    check("OK" in text(c / "T3.TXT") and "FAIL" not in text(c / "T3.TXT"),
          "the replacement tests clean")
    check((c / "X2.BAD").exists() and not (c / "OUT2").exists(),
          "a missing volume stops extraction before anything is written")

    (out / "result.txt").write_text("FAIL\n" if FAILS else "PASS\n")
    print(("%d check(s) failed" % len(FAILS)) if FAILS else
          "DOS16 volume checks passed; evidence: " + str(out))
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
