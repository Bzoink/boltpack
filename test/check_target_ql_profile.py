#!/usr/bin/env python3
"""Host integration checks for the Sinclair QL target profile."""

import argparse
import pathlib
import subprocess
import tempfile


PROFILE = "sinclair-ql-latin1-v1"


def run(*args: str, cwd: pathlib.Path, check: bool = True):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
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
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-ql-target-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        (source / "nested").mkdir(parents=True)
        expected = {
            "caf\u00e9.txt": b"Latin-1 name\n",
            "nested/Gr\u00fc\u00dfe.dat": b"nested Latin-1 name\n",
        }
        for relative, payload in expected.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        create(boltpack, "valid.blt", "source", root)
        run(boltpack, "test", "valid.blt", cwd=root)
        listing = run(boltpack, "list", "valid.blt", cwd=root)
        for name in expected:
            if name not in listing.stdout:
                raise AssertionError(f"archive listing omitted {name!r}")
        appended = source / "append.txt"
        appended.write_bytes(b"append\n")
        run(boltpack, "add", f"--target-profile={PROFILE}", "valid.blt",
            "source/append.txt", cwd=root)
        run(boltpack, "test", "valid.blt", cwd=root)

        bad_sources = {}
        case_collision = root / "case-collision"
        case_collision.mkdir()
        (case_collision / "caf\u00e9.txt").write_bytes(b"lower\n")
        (case_collision / "CAF\u00c9.TXT").write_bytes(b"upper\n")
        bad_sources["case-collision"] = case_collision

        separator_alias = root / "separator-alias"
        (separator_alias / "foo").mkdir(parents=True)
        (separator_alias / "foo_bar").write_bytes(b"flat\n")
        (separator_alias / "foo" / "bar").write_bytes(b"nested\n")
        bad_sources["separator-alias"] = separator_alias

        unrepresentable = root / "unrepresentable"
        unrepresentable.mkdir()
        (unrepresentable / "emoji-\U0001f680.txt").write_bytes(b"bad\n")
        bad_sources["unrepresentable"] = unrepresentable

        long_path = root / "long-path"
        deep = long_path / ("a" * 20) / ("b" * 20)
        deep.mkdir(parents=True)
        (deep / "file.txt").write_bytes(b"too long\n")
        bad_sources["long-path"] = long_path

        for label, bad_source in bad_sources.items():
            archive = f"invalid-{label}.blt"
            result = create(boltpack, archive, bad_source.name, root, False)
            if result.returncode == 0 or (root / archive).exists():
                raise AssertionError(f"QL target profile accepted {label}")

        # Append must validate existing entries before publishing an edit.
        alias_archive = root / "existing-alias.blt"
        run(boltpack, "create", alias_archive.name, "separator-alias", cwd=root)
        before = alias_archive.read_bytes()
        result = run(
            boltpack, "add", f"--target-profile={PROFILE}",
            alias_archive.name, "source/append.txt", cwd=root, check=False,
        )
        if result.returncode == 0 or alias_archive.read_bytes() != before:
            raise AssertionError("QL target profile accepted conflicting existing paths")

    print("Sinclair QL target profile integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
