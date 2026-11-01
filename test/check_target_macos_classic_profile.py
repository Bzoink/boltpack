#!/usr/bin/env python3
"""Host checks for the Classic Mac HFS target profile."""

import argparse
import pathlib
import subprocess
import tempfile


def run(*args: str, cwd: pathlib.Path, check: bool = True):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def create(boltpack: str, root: pathlib.Path, source: str, archive: str,
           check: bool = True):
    return run(
        boltpack, "create", "--target-profile=macos-classic-hfs-v1",
        archive, source, cwd=root, check=check,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-macos-classic-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        (source / "subdir").mkdir(parents=True)
        expected = {
            "plain.txt": b"ASCII\n",
            "caf\u00e9.txt": b"MacRoman name\n",
            "subdir/Gr\u00fc\u00dfe.dat": b"nested MacRoman name\n",
            "a" * 31: b"31-byte HFS component\n",
        }
        for relative, payload in expected.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        create(boltpack, root, "source", "valid.blt")
        run(boltpack, "test", "valid.blt", cwd=root)
        listed = run(boltpack, "list", "valid.blt", cwd=root)
        for relative in expected:
            if relative not in listed.stdout:
                raise AssertionError(f"listing omitted {relative!r}")

        append_file = source / "append.txt"
        append_file.write_bytes(b"append payload\n")
        run(
            boltpack, "add", "--target-profile=macos-classic-hfs-v1",
            "valid.blt", "source/append.txt", cwd=root,
        )
        run(boltpack, "test", "valid.blt", cwd=root)

        invalid_existing = root / "invalid-existing"
        invalid_existing.mkdir()
        (invalid_existing / "emoji-\U0001f680.txt").write_bytes(b"invalid\n")
        run(boltpack, "create", "invalid-existing.blt", "invalid-existing",
            cwd=root)
        before = (root / "invalid-existing.blt").read_bytes()
        result = run(
            boltpack, "add", "--target-profile=macos-classic-hfs-v1",
            "invalid-existing.blt", "source/append.txt", cwd=root,
            check=False,
        )
        if result.returncode == 0 or (root / "invalid-existing.blt").read_bytes() != before:
            raise AssertionError("append accepted an invalid existing HFS path")

        collision = root / "collision"
        collision.mkdir()
        (collision / "caf\u00e9.txt").write_bytes(b"lower\n")
        (collision / "CAF\u00c9.TXT").write_bytes(b"upper\n")
        result = create(boltpack, root, "collision", "collision.blt", False)
        if result.returncode == 0 or (root / "collision.blt").exists():
            raise AssertionError("case-fold collision was accepted")

        bad_cases = {
            "component-too-long": "a" * 32,
            "colon": "volume:name.txt",
            "unmappable": "emoji-\U0001f680.txt",
        }
        for label, name in bad_cases.items():
            bad_source = root / f"bad-{label}"
            bad_source.mkdir()
            (bad_source / name).write_bytes(b"must be rejected\n")
            result = create(
                boltpack, root, bad_source.name, f"{label}.blt", False
            )
            if result.returncode == 0 or (root / f"{label}.blt").exists():
                raise AssertionError(f"accepted invalid HFS name {label}")

    print("Classic Mac HFS target profile integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
