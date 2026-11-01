#!/usr/bin/env python3
"""Run the Win32 GUI's production create-name preflight under Wine."""
import argparse
import pathlib
import subprocess
import tempfile


def wine_path(path):
    return "Z:" + str(path.resolve()).replace("/", "\\")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="bp-win-gui-profile-") as temp:
        root = pathlib.Path(temp)
        valid = root / "valid-tree"
        (valid / "Nested").mkdir(parents=True)
        (valid / "Nested" / "payload.txt").write_bytes(b"profile source\n")
        collision = root / "collision-tree"
        collision.mkdir()
        (collision / "Readme.txt").write_bytes(b"upper\n")
        (collision / "README.TXT").write_bytes(b"lower\n")

        subprocess.run(
            [args.wine, str(args.binary.resolve()), wine_path(valid),
             wine_path(collision)],
            check=True,
            timeout=30,
        )


if __name__ == "__main__":
    main()
