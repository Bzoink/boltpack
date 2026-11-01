#!/usr/bin/env python3
"""Host writer and native Linux mini-reader checks for Linux profiles."""

import argparse
import os
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    parser.add_argument("--mini-cli", required=True)
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())
    mini = str(pathlib.Path(args.mini_cli).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-linux-target-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        source.mkdir()
        expected = {
            "A.txt": b"upper name\n",
            "a.txt": b"lower name\n",
            "caf\u00e9.txt": b"UTF-8 canonical path\n",
            "subdir/nested.bin": bytes(range(256)) * 8,
        }
        for relative, payload in expected.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        posix_archive = "posix.blt"
        run(
            boltpack,
            "create",
            "--target-profile=linux-posix-v1",
            posix_archive,
            source.name,
            cwd=root,
        )
        run(boltpack, "test", posix_archive, cwd=root)
        listing = run(boltpack, "list", posix_archive, cwd=root).stdout
        if "A.txt" not in listing or "a.txt" not in listing:
            raise AssertionError("Linux POSIX profile folded case-sensitive names")

        # The generic POSIX target profile uses the same byte-preserving
        # namespace rules, keeps the full codec set, and validates existing
        # symlink payloads before append.
        posix_source = root / "posix-transfer"
        posix_source.mkdir()
        (posix_source / "payload.txt").write_bytes(b"POSIX profile payload\n")
        (posix_source / "payload-link").symlink_to("payload.txt")
        posix_full_archive = "generic-posix-zstd.blt"
        run(
            boltpack,
            "create",
            "--target-profile=posix-utf8-v1",
            "--zstd",
            posix_full_archive,
            posix_source.name,
            cwd=root,
        )
        run(boltpack, "test", posix_full_archive, cwd=root)
        extra_posix = root / "posix-extra.txt"
        extra_posix.write_bytes(b"append after symlink validation\n")
        run(
            boltpack,
            "add",
            "--target-profile=posix-utf8-v1",
            posix_full_archive,
            extra_posix.name,
            cwd=root,
        )
        run(boltpack, "test", posix_full_archive, cwd=root)
        posix_output = root / "posix-transfer-output"
        posix_output.mkdir()
        run(boltpack, "extract", "-o", str(posix_output), posix_full_archive,
            cwd=root)
        posix_result = posix_output / posix_source.name
        if (posix_result / "payload.txt").read_bytes() != b"POSIX profile payload\n" or \
                not (posix_result / "payload-link").is_symlink() or \
                os.readlink(posix_result / "payload-link") != "payload.txt":
            raise AssertionError("generic POSIX profile transfer changed its link or payload")
        if (posix_output / extra_posix.name).read_bytes() != \
                b"append after symlink validation\n":
            raise AssertionError("generic POSIX profile append lost its payload")

        # Each mini-supported codec must be consumable by the real 32-bit mini
        # reader. Exercise create and append as well as integrity and payload.
        for method_flag in ("-0", "-1", "-2"):
            archive = f"mini-{method_flag[1]}.blt"
            run(
                boltpack,
                "create",
                "--target-profile=linux-mini-v1",
                method_flag,
                archive,
                source.name,
                cwd=root,
            )
            run(mini, "t", archive, cwd=root)
            destination = root / f"out-{method_flag[1]}"
            destination.mkdir()
            run(mini, "x", archive, str(destination), cwd=root)
            for relative, payload in expected.items():
                if (destination / source.name / relative).read_bytes() != payload:
                    raise AssertionError(
                        f"mini reader changed payload {relative!r} with {method_flag}"
                    )

            if method_flag == "-2":
                extra = source / "added.txt"
                extra.write_bytes(b"appended data\n")
                run(
                    boltpack,
                    "add",
                    "--target-profile=linux-mini-v1",
                    archive,
                    str(extra.relative_to(root)),
                    cwd=root,
                )
                run(mini, "t", archive, cwd=root)

        bad_options = (
            ("-F",),
            ("-b", "64k"),
        )
        for index, options in enumerate(bad_options):
            result = run(
                boltpack,
                "create",
                "--target-profile=linux-mini-v1",
                *options,
                f"bad-option-{index}.blt",
                source.name,
                cwd=root,
                check=False,
            )
            if result.returncode == 0 or (root / f"bad-option-{index}.blt").exists():
                raise AssertionError(f"Linux mini profile accepted {options!r}")

        symlink_source = root / "symlink-source"
        symlink_source.mkdir()
        (symlink_source / "link").symlink_to("missing-target")
        result = run(
            boltpack,
            "create",
            "--target-profile=linux-mini-v1",
            "symlink.blt",
            symlink_source.name,
            cwd=root,
            check=False,
        )
        if result.returncode == 0 or (root / "symlink.blt").exists():
            raise AssertionError("Linux mini profile accepted a symlink")

        bad_utf8 = root / "bad-utf8"
        bad_utf8.mkdir()
        fd = os.open(os.fsencode(bad_utf8) + b"/\xff.txt", os.O_CREAT | os.O_WRONLY, 0o600)
        os.close(fd)
        result = run(
            boltpack,
            "create",
            "--target-profile=linux-posix-v1",
            "bad-utf8.blt",
            bad_utf8.name,
            cwd=root,
            check=False,
        )
        if result.returncode == 0 or (root / "bad-utf8.blt").exists():
            raise AssertionError("Linux POSIX profile accepted a non-UTF-8 name")

        huge = root / "huge"
        huge.mkdir()
        with (huge / "sparse.bin").open("wb") as stream:
            stream.truncate(0x100000000)
        result = run(
            boltpack,
            "create",
            "--target-profile=linux-mini-v1",
            "huge.blt",
            huge.name,
            cwd=root,
            check=False,
        )
        if result.returncode == 0 or (root / "huge.blt").exists():
            raise AssertionError("Linux mini profile accepted a 4 GiB file")

    print("Linux POSIX, generic POSIX, and mini target profiles integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
