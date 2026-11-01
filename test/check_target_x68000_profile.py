#!/usr/bin/env python3
"""Linux writer and X68000 host-shim checks for Human68k v3 names."""

import argparse
import os
import pathlib
import subprocess
import tempfile


PROFILE = "x68000-sjis-v1"


def run(*args: str, cwd: pathlib.Path, check: bool = True, env=None):
    result = subprocess.run(args, cwd=cwd, text=True, errors="replace",
                            capture_output=True, env=env)
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def create(boltpack: str, archive: str, source: str, cwd: pathlib.Path,
           check: bool = True):
    return run(boltpack, "create", f"--target-profile={PROFILE}", archive,
               source, cwd=cwd, check=check)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    parser.add_argument("--x68k-host", required=True)
    parser.add_argument("--mini-cli", required=True)
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())
    x68k = str(pathlib.Path(args.x68k_host).resolve())
    mini = str(pathlib.Path(args.mini_cli).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-x68000-target-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        (source / "Nested").mkdir(parents=True)
        expected = {
            "plain.txt": b"ASCII file\n",
            "日本語.bin": b"standard Shift-JIS JIS X 0208\n",
            "trail-―.bin": b"Shift-JIS trail byte 0x5c remains data\n",
            "Nested/かな.dat": b"nested Shift-JIS path\n",
            "123456789012345678.xyz": b"18.3 byte boundary\n",
        }
        assert "―".encode("shift_jis") == b"\x81\\"
        for relative, payload in expected.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        create(boltpack, "valid.blt", "source", root)
        run(boltpack, "test", "valid.blt", cwd=root)
        run(mini, "t", "valid.blt", cwd=root)
        listing = run(boltpack, "list", "valid.blt", cwd=root).stdout
        for name in expected:
            if name not in listing:
                raise AssertionError(f"archive listing omitted {name!r}")

        output = root / "x68k-output"
        x68k_env = os.environ.copy()
        x68k_env["BP_TZ_OFFSET"] = "0"
        extracted = run(x68k, "x", "-o", str(output), "valid.blt", cwd=root,
                        check=False, env=x68k_env)
        if extracted.returncode not in (0, 1):
            raise AssertionError("X68000 frontend failed profile extraction:\n" +
                                 extracted.stdout + extracted.stderr)
        for relative, payload in expected.items():
            raw_relative = relative.encode("shift_jis")
            raw_path = os.fsencode(output) + b"/source/" + raw_relative
            with open(raw_path, "rb") as actual:
                found = actual.read()
            if found != payload:
                raise AssertionError(f"X68000 host shim payload mismatch: {relative!r}")

        append_file = source / "append.bin"
        append_file.write_bytes(b"append payload\n")
        run(boltpack, "add", f"--target-profile={PROFILE}", "valid.blt",
            "source/append.bin", cwd=root)
        run(boltpack, "test", "valid.blt", cwd=root)

        invalid_names = {
            "stem-too-long": "A" * 19,
            "extension-too-long": "file.long",
            "multiple-dots": "two.dots.ext",
            "reserved-colon": "bad:name",
            "unmappable": "emoji-\U0001f680",
        }
        for label, name in invalid_names.items():
            bad = root / f"bad-{label}"
            bad.mkdir()
            (bad / name).write_bytes(b"must be refused\n")
            archive = f"invalid-{label}.blt"
            result = create(boltpack, archive, bad.name, root, False)
            if result.returncode == 0 or (root / archive).exists():
                raise AssertionError(f"X68000 profile accepted {label}")

        long_path = root / "bad-relative-path"
        cursor = long_path
        for _ in range(12):
            cursor = cursor / "123456789012345678.ext"
        cursor.mkdir(parents=True)
        (cursor / "x").write_bytes(b"path exceeds the frontend limit\n")
        result = create(boltpack, "invalid-path.blt", long_path.name, root,
                        False)
        if result.returncode == 0 or (root / "invalid-path.blt").exists():
            raise AssertionError("X68000 profile accepted an overlong path")

        collision = root / "case-collision"
        collision.mkdir()
        (collision / "Readme.txt").write_bytes(b"lower\n")
        (collision / "README.TXT").write_bytes(b"upper\n")
        result = create(boltpack, "invalid-case.blt", "case-collision", root,
                        False)
        if result.returncode == 0 or (root / "invalid-case.blt").exists():
            raise AssertionError("X68000 profile accepted ASCII case aliases")

        # A separately added child may conflict with an existing file even
        # though the host filesystem cannot contain both shapes at once.
        (root / "node").write_bytes(b"existing file\n")
        create(boltpack, "prefix.blt", "node", root)
        (root / "node").unlink()
        (root / "node").mkdir()
        (root / "node" / "child.bin").write_bytes(b"new child\n")
        before = (root / "prefix.blt").read_bytes()
        result = run(boltpack, "add", f"--target-profile={PROFILE}",
                     "prefix.blt", "node/child.bin", cwd=root, check=False)
        if result.returncode == 0 or (root / "prefix.blt").read_bytes() != before:
            raise AssertionError("X68000 profile accepted file/directory prefix conflict")

        for options in (("-F",), ("-b", "32k")):
            result = run(boltpack, "create", *options,
                         f"--target-profile={PROFILE}", "invalid-limit.blt",
                         "source", cwd=root, check=False)
            if result.returncode == 0 or (root / "invalid-limit.blt").exists():
                raise AssertionError(f"X68000 profile accepted {options}")

    print("X68000 Shift-JIS target profile integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
