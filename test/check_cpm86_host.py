#!/usr/bin/env python3
"""Exercise the CP/M-86 frontend's flat 8.3 policy on a host build."""
import subprocess
import sys
import tempfile
from pathlib import Path


def run(binary, *args, cwd):
    return subprocess.run([str(binary), *args], cwd=cwd, capture_output=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="bp_cpm86_") as directory:
        root = Path(directory)
        (root / "one.txt").write_bytes(b"first CP/M file\n")
        (root / "two.txt").write_bytes(b"second CP/M file\n")

        result = run(binary, "c", "GOOD.BLT", "one.txt", "two.txt", cwd=root)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        assert run(binary, "t", "GOOD.BLT", cwd=root).returncode == 0
        listing = run(binary, "l", "GOOD.BLT", cwd=root)
        assert listing.returncode == 0, listing.stderr.decode(errors="replace")
        assert b"ONE.TXT" in listing.stdout and b"TWO.TXT" in listing.stdout

        out = root / "extract"
        out.mkdir()
        result = run(binary, "x", str(root / "GOOD.BLT"), cwd=out)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        assert (out / "ONE.TXT").read_bytes() == b"first CP/M file\n"
        assert (out / "TWO.TXT").read_bytes() == b"second CP/M file\n"

        (root / "TOOLONGNAME.TXT").write_bytes(b"long name\n")
        result = run(binary, "c", "LONG.BLT", "TOOLONGNAME.TXT", cwd=root)
        assert result.returncode != 0 and not (root / "LONG.BLT").exists()

        (root / "REPORT.TXT").write_bytes(b"upper\n")
        (root / "report.txt").write_bytes(b"lower\n")
        result = run(binary, "c", "DUP.BLT", "REPORT.TXT", "report.txt", cwd=root)
        assert result.returncode != 0 and not (root / "DUP.BLT").exists()

        result = run(binary, "c", "ONE.TXT", "one.txt", cwd=root)
        assert result.returncode != 0
        assert (root / "one.txt").read_bytes() == b"first CP/M file\n"

        existing = root / "EXIST.BLT"
        existing.write_bytes(b"preserve me\n")
        result = run(binary, "c", "EXIST.BLT", "one.txt", cwd=root)
        assert result.returncode != 0
        assert existing.read_bytes() == b"preserve me\n"

    print("CP/M-86 host frontend projection checks passed")


if __name__ == "__main__":
    main()
