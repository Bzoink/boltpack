#!/usr/bin/env python3
"""Volume sets (archive format 7) on the Linux mini CLI.

bpmini writes and reads volume sets with -v SIZE; this exchanges sets with
the full Linux CLI in both directions, reads from any volume, and checks
the refusals (missing volume, numbered archive name, -v outside create).
"""
import filecmp
import os
from pathlib import Path
import random
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MINI = Path(os.environ.get("BP_TEST_LINUXMINI", ROOT / "build/linux32mini/bpmini"))
RUNNER = shlex.split(os.environ.get("BP_TEST_LINUXMINI_RUNNER", ""))
FULL = ROOT / "build/linux/boltpack"
FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("  FAIL  " + what)


def same_tree(a, b):
    cmp = filecmp.dircmp(a, b)
    if cmp.left_only or cmp.right_only or cmp.diff_files or cmp.funny_files:
        return False
    for f in cmp.common_files:
        if not filecmp.cmp(Path(a) / f, Path(b) / f, shallow=False):
            return False
    return all(same_tree(Path(a) / d, Path(b) / d) for d in cmp.common_dirs)


def mini(args, cwd):
    p = subprocess.run(RUNNER + [str(MINI)] + args, cwd=cwd, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


def full(args, cwd):
    p = subprocess.run([str(FULL)] + args, cwd=cwd, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


def main():
    work = Path(tempfile.mkdtemp(prefix="bpminivol_"))
    try:
        rnd = random.Random(3)
        src = work / "src"
        (src / "sub" / "deeper").mkdir(parents=True)
        (src / "text.txt").write_bytes(b"mini volume set text " * 6000)
        (src / "sub" / "random.bin").write_bytes(bytes(rnd.getrandbits(8) for _ in range(150000)))
        (src / "sub" / "deeper" / "mixed.dat").write_bytes(
            (b"mixed block " * 300 + bytes(rnd.getrandbits(8) for _ in range(4000))) * 8)
        for i in range(8):
            (src / "sub" / ("s%d.txt" % i)).write_bytes(b"small %d\n" % i * (i + 1))

        print("Linux full CLI writes, bpmini reads")
        d = work / "a"
        d.mkdir()
        shutil.copytree(src, d / "src")
        rc, out, err = full(["c", "-v", "60K", "-0", "set.bolt", "src"], d)
        vols = sorted(n for n in os.listdir(d) if n.startswith("set."))
        check(rc == 0 and len(vols) >= 4, "full CLI made a set (%s)" % vols)
        for given in ("set.bolt", "set.001", "set.000"):
            rc, out, err = mini(["l", given], d)
            check(rc == 0 and "random.bin" in out, "bpmini lists via %s" % given)
        rc, out, err = mini(["t", "set.002"], d)
        check(rc == 0, "bpmini tests via set.002 (%s)" % err.strip()[-120:])
        rc, out, err = mini(["i", "set.bolt"], d)
        check(rc == 0 and "Volumes:" in out and "(all volumes)" in out,
              "bpmini info shows the volumes")
        rc, out, err = mini(["x", "set.bolt", "out"], d)
        check(rc == 0 and same_tree(d / "src", d / "out" / "src"),
              "bpmini extracts the set (%s)" % err.strip()[-120:])

        print("bpmini writes, Linux full CLI reads")
        d = work / "b"
        d.mkdir()
        shutil.copytree(src, d / "src")
        rc, out, err = mini(["c", "-v", "60K", "-m0", "mset.bolt", "src"], d)
        vols = sorted(n for n in os.listdir(d) if n.startswith("mset."))
        check(rc == 0 and len(vols) >= 4 and vols[-1] == "mset.bolt" and
              all(n == "mset.%03d" % i for i, n in enumerate(vols[:-1])),
              "bpmini made a set (%s) %s" % (vols, err.strip()[-120:]))
        check(all(os.path.getsize(d / n) <= 60000 for n in vols),
              "every volume is at most 60K")
        check(not [n for n in os.listdir(d) if n.endswith(".tmp") or n.startswith("bpvol")],
              "no staging files left behind")
        rc, out, err = full(["t", "mset.bolt"], d)
        check(rc == 0, "full CLI tests the bpmini set (%s)" % err.strip()[-120:])
        rc, out, err = full(["x", "-o", "out", "mset.001"], d)
        check(rc in (0, 1) and same_tree(d / "src", d / "out" / "src"),
              "full CLI extracts the bpmini set")
        rc, out, err = mini(["c", "-v", "60K", "-m1", "mset.bolt", "src/text.txt"], d)
        vols2 = sorted(n for n in os.listdir(d) if n.startswith("mset."))
        check(rc == 0 and len(vols2) < len(vols), "a smaller set removes stale volumes (%d -> %d)"
              % (len(vols), len(vols2)))

        print("refusals")
        d = work / "c"
        d.mkdir()
        shutil.copytree(src, d / "src")
        full(["c", "-v", "60K", "-0", "m.bolt", "src"], d)
        os.remove(d / "m.001")
        rc, out, err = mini(["t", "m.bolt"], d)
        check(rc != 0 and "m.001" in err, "a missing volume is named (%s)" % err.strip()[-120:])
        rc, out, err = mini(["c", "-v", "60K", "n.001", "src"], d)
        check(rc != 0 and "cannot name a volume set" in err, "a numbered archive name is refused")
        rc, out, err = mini(["l", "-v", "60K", "m.bolt"], d)
        check(rc != 0 and "-v applies to create" in err, "-v only with create")
        rc, out, err = mini(["c", "-v", "15K", "x.bolt", "src"], d)
        check(rc != 0 and "outside" in err, "volumes below 16K are refused")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if FAILS:
        print("%d check(s) failed" % len(FAILS))
        return 1
    print("linuxmini volume checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
