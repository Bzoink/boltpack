#!/usr/bin/env python3
"""Volume sets (archive format 7) with the DJGPP DOS CLI on DOSBox-X.

Writes a set onto a FAT12 1.44 MB floppy image (answering the --pause
prompts from a file), reads it back from any volume, replaces it with a
smaller set, checks the refusal when a volume is missing, and exchanges sets
with the Linux CLI in both directions.

DOS runs without long file names, so DJGPP reports names in lower case and
the DOS CLI stores them that way; extracting its own set then also checks
that lower-case 8.3 names are accepted.  Disk swapping itself is covered by
test/check_volumes.py against the DOS host build; DOSBox-X cannot feed the
prompts one Enter at a time.

Usage:
    python3 test/check_djgpp_dosboxx_volumes.py --cwsdpmi PATH/CWSDPMI.EXE
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


def dosbox(args, cdir, image, commands, log, limit=420, lfn=False):
    """Run commands in one DOSBox-X session.  The emulated floppy is slow
    (about 50 s per 1.4 MB), and long sessions have been seen to stall after
    the last command, so sessions are kept short, DOSBox-X ends each one
    after `limit` seconds, and a marker shows that every command returned."""
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    marker = cdir / "DONE.TXT"
    if marker.exists():
        marker.unlink()
    cmd = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch",
           "-time-limit", str(limit), "-set", "dos lfn=" + ("true" if lfn else "false"),
           "-c", "mount c " + str(cdir),
           "-c", "imgmount a " + str(image) + " -t floppy", "-c", "c:"]
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dosbox-x", default="dosbox-x")
    ap.add_argument("--cwsdpmi", type=Path, required=True)
    ap.add_argument("--output", type=Path,
                    help="new directory for images and logs (kept)")
    args = ap.parse_args()
    exe = ROOT / "build/dos-djgpp/boltpack.exe"
    linux = ROOT / "build/linux/boltpack"
    if not exe.is_file():
        raise SystemExit("build the DJGPP executable first: make dos-djgpp")
    out = args.output.resolve() if args.output else Path(
        tempfile.mkdtemp(prefix="boltpack-djgpp-volumes-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)

    c = out / "c"
    (c / "DOCS" / "SUB").mkdir(parents=True)
    rnd = random.Random(1440)
    (c / "DOCS" / "RANDOM.BIN").write_bytes(
        bytes(rnd.getrandbits(8) for _ in range(1100000)))
    words = b"diskette volume set for DOS archive restore "
    (c / "DOCS" / "SUB" / "TEXT.TXT").write_bytes(words * 9000)
    (c / "DOCS" / "SMALL.TXT").write_bytes(b"small file\r\n")
    shutil.copyfile(exe, c / "BP.EXE")
    shutil.copyfile(args.cwsdpmi.resolve(), c / "CWSDPMI.EXE")
    (c / "KEYS.TXT").write_bytes(b"\r\n" * 20)

    # a set made by Linux, for DOS to read
    run([str(linux), "c", "-2", "-v", "150K", "LX.BLT", "DOCS"], cwd=c)
    linux_vols = sorted(p.name for p in c.glob("LX.*"))

    floppy = out / "floppy.img"
    run(["mformat", "-i", str(floppy), "-C", "-f", "1440", "::"])

    dosbox(args, c, floppy, [
        "BP.EXE c -v 700K --pause A:\\BACKUP.BLT DOCS < KEYS.TXT > W1.TXT",
        "BP.EXE l A:\\BACKUP.000 > L1.TXT",
        "BP.EXE i A:\\BACKUP.BLT > I1.TXT",
    ], out / "dosbox-write.log")
    dosbox(args, c, floppy, [
        "BP.EXE t A:\\BACKUP.BLT > T1.TXT",
        "BP.EXE x -o OUT1 A:\\BACKUP.000 < NUL > X1.TXT",
    ], out / "dosbox-read.log")
    dosbox(args, c, floppy, [
        "BP.EXE x -o OUTLX LX.000 < NUL > XLX.TXT",
        "BP.EXE c -v 300K SETC.BLT DOCS > W2.TXT",
    ], out / "dosbox-exchange.log")

    after_first = mdir(floppy)
    check(after_first == ["BACKUP.000", "BACKUP.BLT"],
          "the set fills the floppy as BACKUP.000 and BACKUP.BLT (%s)" % after_first)
    w1 = (c / "W1.TXT").read_text(errors="replace")
    check("2 volumes" in w1, "create reports 2 volumes")
    check("DOCS/RANDOM.BIN" in (c / "L1.TXT").read_text(errors="replace").upper(),
          "list through BACKUP.000 reads the table from BACKUP.BLT")
    check("files OK" in (c / "T1.TXT").read_text(errors="replace"),
          "test passes")
    check(same_tree(c / "DOCS", c / "OUT1" / "DOCS"),
          "extract through BACKUP.000 restores the tree")
    info = (c / "I1.TXT").read_text(errors="replace")
    check("Volumes:        2" in info and "Format version: 7" in info,
          "info shows format 7 and 2 volumes")
    check(same_tree(c / "DOCS", c / "OUTLX" / "DOCS"),
          "DOS extracts the Linux-made set (%d volumes)" % len(linux_vols))

    # Linux reads the DOS-made sets
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
    check(rc.returncode == 0 and len(setc) >= 3,
          "Linux tests the DOS set written to C: (%d volumes)" % len(setc))

    # replace with a smaller set; take a volume away
    (c / "SETC.002").rename(c / "AWAY.002")
    dosbox(args, c, floppy, [
        # small enough to be staged beside the old set on the full floppy
        "BP.EXE c -v 1.44M A:\\BACKUP.BLT DOCS\\SUB > W3.TXT",
        "BP.EXE t A:\\BACKUP.BLT > T3.TXT",
        "BP.EXE x -o OUT2 SETC.BLT < NUL > X2.TXT",
    ], out / "dosbox-replace.log")
    after_second = mdir(floppy)
    w3 = (c / "W3.TXT").read_text(errors="replace")
    check(after_second == ["BACKUP.BLT"] and "1 volume" in w3,
          "a one-volume set replaces the two-volume set and its leftover "
          "BACKUP.000 is removed (%s)" % after_second)
    check("files OK" in (c / "T3.TXT").read_text(errors="replace"),
          "the replacement tests clean")
    check(not (c / "OUT2").exists(),
          "a missing volume stops extraction before anything is written")
    # With long file names DJGPP reports different identities for a handle
    # and a path (invented inode numbers, the creation time); extraction must
    # still publish its staged files and add/delete must accept the archive.
    shutil.copyfile(c / "LX.BLT", c / "LFNSET.BLT")
    for name in sorted(c.glob("LX.0*")):
        shutil.copyfile(name, c / ("LFNSET" + name.suffix))
    (c / "EXTRA.TXT").write_bytes(b"added under LFN\r\n")
    dosbox(args, c, floppy, [
        "BP.EXE x -o LFNOUT LFNSET.000 < NUL > X4.TXT",
        "BP.EXE c ONE.BLT DOCS\\SMALL.TXT > C4.TXT",
        "BP.EXE a ONE.BLT EXTRA.TXT > A4.TXT",
        "BP.EXE d ONE.BLT EXTRA.TXT > D4.TXT",
        "BP.EXE t ONE.BLT > T4.TXT",
    ], out / "dosbox-lfn.log", lfn=True)
    check(same_tree(c / "DOCS", c / "LFNOUT" / "DOCS") and
          not list((c / "LFNOUT").rglob("BP*.TMP")),
          "with long file names, extraction publishes every file and leaves no stage")
    check("1 file added" in (c / "A4.TXT").read_text(errors="replace") and
          "deleted" in (c / "D4.TXT").read_text(errors="replace") and
          "files OK" in (c / "T4.TXT").read_text(errors="replace"),
          "with long file names, add and delete accept the archive")

    (out / "result.txt").write_text("FAIL\n" if FAILS else "PASS\n")
    print(("%d check(s) failed" % len(FAILS)) if FAILS else
          "DJGPP volume checks passed; evidence: " + str(out))
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
