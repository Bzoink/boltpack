#!/usr/bin/env python3
"""Host integration checks for the conservative AmigaOS 68k profile."""

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


def create(
    boltpack: str, root: pathlib.Path, source: str, archive: str, check: bool = True
):
    return run(
        boltpack,
        "create",
        "--target-profile=amigaos-68k-latin1",
        archive,
        source,
        cwd=root,
        check=check,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-amiga-target-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        (source / "subdir").mkdir(parents=True)
        expected = {
            "plain.txt": b"ascii\n",
            "caf\u00e9.txt": b"Latin-1 name\n",
            "subdir/Gr\u00fc\u00dfe.dat": b"nested Latin-1 name\n",
        }
        for relative, payload in expected.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        create(boltpack, root, source.name, "valid.blt")
        run(boltpack, "test", "valid.blt", cwd=root)
        listed = run(boltpack, "list", "valid.blt", cwd=root)
        for relative in expected:
            if relative not in listed.stdout:
                raise AssertionError(f"archive listing omitted {relative!r}")

        added = source / "append.txt"
        added.write_bytes(b"append path\n")
        run(
            boltpack,
            "add",
            "--target-profile=amigaos-68k-latin1",
            "valid.blt",
            str(added.relative_to(root)),
            cwd=root,
        )
        run(boltpack, "test", "valid.blt", cwd=root)

        collision = root / "collision"
        collision.mkdir()
        (collision / "Foo.txt").write_bytes(b"upper\n")
        (collision / "foo.TXT").write_bytes(b"lower\n")
        (collision / "caf\u00e9.dat").write_bytes(b"accent lower\n")
        (collision / "CAF\u00c9.DAT").write_bytes(b"accent upper\n")
        result = create(
            boltpack, root, collision.name, "collision.blt", check=False
        )
        if result.returncode == 0 or (root / "collision.blt").exists():
            raise AssertionError("AmigaOS profile accepted a case-fold collision")

        deep = root / "deep"
        deep.mkdir()
        current = deep
        for index in range(9):
            current = current / (f"d{index}" + "x" * 28)
            current.mkdir()
        (current / "file.txt").write_bytes(b"too deep\n")
        result = create(boltpack, root, deep.name, "deep.blt", check=False)
        if result.returncode == 0 or (root / "deep.blt").exists():
            raise AssertionError("AmigaOS profile accepted a path over 255 bytes")

        bad_cases = {
            "long": "a" * 31,
            "volume": "VOL:NAME.TXT",
            "unmappable": "emoji-\U0001f680.txt",
        }
        for label, name in bad_cases.items():
            bad_source = root / f"bad-{label}"
            bad_source.mkdir()
            (bad_source / name).write_bytes(b"must be rejected\n")
            result = create(
                boltpack, root, bad_source.name, f"bad-{label}.blt", check=False
            )
            if result.returncode == 0 or (root / f"bad-{label}.blt").exists():
                raise AssertionError(f"AmigaOS profile accepted {label}: {result}")

    print("AmigaOS 68k target profile integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
