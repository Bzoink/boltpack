#!/usr/bin/env python3
"""Verify path-based POSIX wrappers refuse extraction before creating output."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--refusing-boltpack", action="append", required=True,
                        type=Path, help="wrapper binary expected to refuse extraction")
    parser.add_argument("--linux-boltpack", required=True, type=Path)
    args = parser.parse_args()
    refusing = [path.resolve() for path in args.refusing_boltpack]
    linux = args.linux_boltpack.resolve()

    with tempfile.TemporaryDirectory(prefix="bp_path_refuse_") as temp:
        root = Path(temp)
        src = root / "src"
        src.mkdir()
        (src / "payload.txt").write_bytes(b"safe extraction refusal\n")
        archive = "sample.blt"
        created = run([str(linux), "c", archive, "src"], root)
        assert created.returncode == 0, created.stderr + created.stdout

        for binary in refusing:
            tested = run([str(binary), "t", archive], root)
            assert tested.returncode == 0, tested.stderr + tested.stdout

        for index, binary in enumerate(refusing):
            output = root / ("must-not-exist-%d" % index)
            dry_run = run([str(binary), "x", "--dry-run", "-o", str(output),
                           archive], root)
            assert dry_run.returncode == 0, dry_run.stderr + dry_run.stdout
            assert not output.exists(), "dry-run created the output root"

            extracted = run([str(binary), "x", "-o", str(output), archive], root)
            assert extracted.returncode != 0, "unsafe extraction unexpectedly proceeded"
            assert "cannot anchor output paths safely" in extracted.stderr
            assert not output.exists(), "wrapper created output before refusing extraction"

            default_output = run([str(binary), "x", archive], root)
            assert default_output.returncode != 0, "default extraction unexpectedly proceeded"
            assert "cannot anchor output paths safely" in default_output.stderr
            assert (src / "payload.txt").read_bytes() == b"safe extraction refusal\n"
            assert not list(root.rglob(".boltpack-*")), "refused extraction left a stage"

    print("Path-based wrapper extraction refusal and dry-run checks passed")


if __name__ == "__main__":
    main()
