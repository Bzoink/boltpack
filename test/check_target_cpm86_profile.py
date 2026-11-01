#!/usr/bin/env python3
"""Cross-check the Linux CP/M-86 profile with the host CP/M-86 frontend."""

import argparse
import pathlib
import subprocess
import tempfile


def run(*args: str, cwd: pathlib.Path, check: bool = True):
    result = subprocess.run(
        args, cwd=cwd, text=True, errors="replace", capture_output=True
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def create(boltpack: str, root: pathlib.Path, archive: str, *names: str,
           options: tuple[str, ...] = (), check: bool = True):
    return run(
        boltpack,
        "create",
        "--target-profile=cpm86-83",
        *options,
        archive,
        *names,
        cwd=root,
        check=check,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    parser.add_argument("--cpm86-cli", required=True)
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())
    cpm86 = str(pathlib.Path(args.cpm86_cli).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-cpm86-target-") as tmp:
        root = pathlib.Path(tmp)
        (root / "one.txt").write_bytes(b"CP/M payload one\n")
        (root / "two.dat").write_bytes(bytes(range(256)) * 12)
        create(boltpack, root, "good.blt", "one.txt", "two.dat")
        run(cpm86, "t", "good.blt", cwd=root)
        output = root / "out"
        output.mkdir()
        run(cpm86, "x", str(root / "good.blt"), cwd=output)
        if (output / "ONE.TXT").read_bytes() != b"CP/M payload one\n":
            raise AssertionError("CP/M-86 extraction changed the first payload")
        if (output / "TWO.DAT").read_bytes() != bytes(range(256)) * 12:
            raise AssertionError("CP/M-86 extraction changed the second payload")

        (root / "three.txt").write_bytes(b"append\n")
        run(
            boltpack,
            "add",
            "--target-profile=cpm86-83",
            "good.blt",
            "three.txt",
            cwd=root,
        )
        run(cpm86, "t", "good.blt", cwd=root)

        run(boltpack, "create", "host-attrs.blt", "one.txt", cwd=root)
        before = (root / "host-attrs.blt").read_bytes()
        result = run(
            boltpack,
            "add",
            "--target-profile=cpm86-83",
            "host-attrs.blt",
            "two.dat",
            cwd=root,
            check=False,
        )
        if result.returncode == 0 or (root / "host-attrs.blt").read_bytes() != before:
            raise AssertionError("CP/M-86 append accepted unrepresentable host attributes")

        bad = {
            "long": ("TOOLONGNAME.TXT",),
            "unicode": ("caf\u00e9.txt",),
            "nested": ("sub/file.txt",),
            "directory": ("sub",),
        }
        (root / "TOOLONGNAME.TXT").write_bytes(b"bad\n")
        (root / "caf\u00e9.txt").write_bytes(b"bad\n")
        (root / "sub").mkdir()
        (root / "sub" / "file.txt").write_bytes(b"bad\n")
        for label, names in bad.items():
            result = create(
                boltpack, root, f"bad-{label}.blt", *names, check=False
            )
            if result.returncode == 0 or (root / f"bad-{label}.blt").exists():
                raise AssertionError(f"CP/M-86 profile accepted {label} input")

        (root / "REPORT.TXT").write_bytes(b"first\n")
        (root / "report.txt").write_bytes(b"second\n")
        result = create(
            boltpack,
            root,
            "collision.blt",
            "REPORT.TXT",
            "report.txt",
            check=False,
        )
        if result.returncode == 0 or (root / "collision.blt").exists():
            raise AssertionError("CP/M-86 profile accepted projected name collision")

        for label, options in (
            ("codec", ("-2",)),
            ("block", ("-b", "64k")),
        ):
            result = create(
                boltpack,
                root,
                f"bad-{label}.blt",
                "one.txt",
                options=options,
                check=False,
            )
            if result.returncode == 0 or (root / f"bad-{label}.blt").exists():
                raise AssertionError(f"CP/M-86 profile accepted {label} option")

        sparse = root / "HUGE.BIN"
        with sparse.open("wb") as stream:
            stream.truncate(0x80000000)
        result = create(boltpack, root, "huge.blt", sparse.name, check=False)
        if result.returncode == 0 or (root / "huge.blt").exists():
            raise AssertionError("CP/M-86 profile accepted a file over 2 GiB")

        many = root / "many"
        many.mkdir()
        many_names = []
        for index in range(257):
            name = f"F{index:07d}.DAT"
            (many / name).write_bytes(b"x")
            many_names.append(name)
        result = create(
            boltpack, many, "many.blt", *many_names, check=False
        )
        if result.returncode == 0 or (many / "many.blt").exists():
            raise AssertionError("CP/M-86 profile accepted over 256 files")

    print("CP/M-86 target profile integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
