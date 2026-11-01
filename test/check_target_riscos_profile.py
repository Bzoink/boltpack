#!/usr/bin/env python3
"""Host integration checks for the RISC OS Latin-1 target profile."""

import argparse
import pathlib
import subprocess
import tempfile


PROFILE = "riscos-latin1-v1"


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
    parser.add_argument("--riscos-host", required=True)
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())
    riscos_host = str(pathlib.Path(args.riscos_host).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-riscos-target-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        (source / "Nested").mkdir(parents=True)
        expected = {
            "plain": b"ASCII name\n",
            "caf\u00e9": b"Latin-1 name\n",
            "Nested/Gr\u00fc\u00dfe": b"nested Latin-1 name\n",
            "1234567890": b"ten-byte leaf\n",
        }
        for relative, payload in expected.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        create(boltpack, "valid.blt", "source", root)
        run(boltpack, "test", "valid.blt", cwd=root)
        listing = run(boltpack, "list", "valid.blt", cwd=root).stdout
        for name in expected:
            if name not in listing:
                raise AssertionError(f"listing omitted {name!r}")

        appended = source / "append"
        appended.write_bytes(b"append\n")
        run(boltpack, "add", f"--target-profile={PROFILE}", "valid.blt",
            "source/append", cwd=root)
        run(boltpack, "test", "valid.blt", cwd=root)

        # Interchange Profile 1 is part of this profile's codec and block
        # ceiling, so reject Fluff and 32 KiB blocks before archive creation.
        for options in (("-F",), ("-b", "32k")):
            archive = "invalid-codec.blt"
            result = run(boltpack, "create", *options,
                         f"--target-profile={PROFILE}", archive, "source",
                         cwd=root, check=False)
            if result.returncode == 0 or (root / archive).exists():
                raise AssertionError(f"RISC OS profile accepted {options}")

        # RISC OS uses FileSwitch case folding and dot as the path separator.
        collision = root / "case-collision"
        collision.mkdir()
        (collision / "caf\u00e9").write_bytes(b"lower\n")
        (collision / "CAF\u00c9").write_bytes(b"upper\n")
        bad_sources = {"case-collision": collision}

        invalid_names = {
            "dot-separator-alias": "dir.name",
            "component-too-long": "12345678901",
            "reserved-colon": "file:name",
            "unmappable": "emoji-\U0001f680",
        }
        for label, name in invalid_names.items():
            bad = root / f"bad-{label}"
            bad.mkdir()
            (bad / name).write_bytes(b"must be refused\n")
            bad_sources[label] = bad

        long_path = root / "bad-relative-path"
        cursor = long_path
        for _ in range(26):
            cursor = cursor / "abcdefghij"
        cursor.mkdir(parents=True)
        (cursor / "f").write_bytes(b"path exceeds 255 bytes\n")
        bad_sources["relative-path-too-long"] = long_path

        for label, bad_source in bad_sources.items():
            archive = f"invalid-{label}.blt"
            result = create(boltpack, archive, bad_source.name, root, False)
            if result.returncode == 0 or (root / archive).exists():
                raise AssertionError(f"RISC OS profile accepted {label}")

        link_source = root / "link-source"
        link_source.mkdir()
        (link_source / "link").symlink_to("target")
        result = create(boltpack, "invalid-link.blt", "link-source", root,
                        False)
        if result.returncode == 0 or (root / "invalid-link.blt").exists():
            raise AssertionError("RISC OS profile accepted a symlink")

        # An archive made without the profile must also be rejected on append
        # if its current members cannot be represented by FileCore.
        dot_source = root / "dot-source"
        dot_source.mkdir()
        (dot_source / "dir.name").write_bytes(b"existing invalid name\n")
        run(boltpack, "create", "existing-invalid.blt", "dot-source", cwd=root)
        before = (root / "existing-invalid.blt").read_bytes()
        result = run(boltpack, "add", f"--target-profile={PROFILE}",
                     "existing-invalid.blt", "source/append", cwd=root,
                     check=False)
        if result.returncode == 0 or \
                (root / "existing-invalid.blt").read_bytes() != before:
            raise AssertionError("append accepted an invalid existing RISC OS path")

        # The Linux writer cannot create RISC OS filetype/attribute metadata.
        # It must refuse to append under this profile if that metadata already
        # exists, because silently carrying it is not the declared policy.
        native = root / "native-writer"
        (native / "tree" / "nested").mkdir(parents=True)
        (native / "tree" / "nested" / "payload").write_bytes(b"native\n")
        (native / "tree" / "nested" / "readme").write_bytes(b"readme\n")
        (native / "new").write_bytes(b"append\n")
        run(riscos_host, cwd=native)
        native_archive = native / "archive"
        before = native_archive.read_bytes()
        result = run(boltpack, "add", f"--target-profile={PROFILE}",
                     "native-writer/archive", "source/append", cwd=root,
                     check=False)
        if result.returncode == 0 or native_archive.read_bytes() != before:
            raise AssertionError("RISC OS profile accepted existing native metadata")

    print("RISC OS Latin-1 target profile integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
