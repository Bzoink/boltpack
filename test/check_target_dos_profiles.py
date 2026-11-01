#!/usr/bin/env python3
"""Host integration checks for legacy DOS-family writer profiles."""

import argparse
import pathlib
import subprocess
import tempfile


PROFILES = (
    "dos-cp850-83",
    "dos-cp437-83",
    "dos-cp850-lfn-v1",
    "dos16-cp850-mini",
    "win16-cp437-mini",
    "os2-16-cp850-mini",
    "os2-fat-cp850",
)
CP850_PROFILES = {
    "dos-cp850-83",
    "dos-cp850-lfn-v1",
    "dos16-cp850-mini",
    "os2-16-cp850-mini",
    "os2-fat-cp850",
}


def run(*args: str, cwd: pathlib.Path, check: bool = True):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    args = parser.parse_args()
    boltpack = str(pathlib.Path(args.boltpack).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-dos-target-") as tmp:
        root = pathlib.Path(tmp)
        common = {
            "plain.txt": b"plain\n",
            "ü.txt": b"shared CP437/CP850 character\n",
            "SUBDIR/NESTED.DAT": b"nested\n",
        }
        for profile_index, profile in enumerate(PROFILES):
            source = root / f"S{profile_index}"
            files = dict(common)
            if profile in CP850_PROFILES:
                files["¤.TXT"] = b"CP850-only character\n"
            else:
                files["Γ.TXT"] = b"CP437-only character\n"
            if profile == "dos-cp850-lfn-v1":
                files["Longer Name with spaces - Grüße.txt"] = b"DOS LFN\n"
            for relative, data in files.items():
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            archive = f"{profile}.blt"
            run(
                boltpack,
                "create",
                f"--target-profile={profile}",
                archive,
                source.name,
                cwd=root,
            )
            run(boltpack, "test", archive, cwd=root)
            if profile == "dos-cp850-lfn-v1":
                appended = source / "Another Long Name.txt"
                appended.write_bytes(b"DOS LFN append\n")
                run(
                    boltpack, "add", f"--target-profile={profile}", archive,
                    str(appended.relative_to(root)), cwd=root,
                )
                run(boltpack, "test", archive, cwd=root)

        for profile in PROFILES:
            bad_source = root / f"bad-{profile}"
            bad_source.mkdir()
            (bad_source / "CON.TXT").write_bytes(b"reserved DOS device\n")
            result = run(
                boltpack,
                "create",
                f"--target-profile={profile}",
                f"bad-{profile}.blt",
                bad_source.name,
                cwd=root,
                check=False,
            )
            if result.returncode == 0 or (root / f"bad-{profile}.blt").exists():
                raise AssertionError(f"{profile} accepted reserved DOS device name")

        collision = root / "collision"
        collision.mkdir()
        (collision / "Foo.TXT").write_bytes(b"first\n")
        (collision / "foo.txt").write_bytes(b"second\n")
        for profile in PROFILES:
            result = run(
                boltpack,
                "create",
                f"--target-profile={profile}",
                f"collision-{profile}.blt",
                "collision",
                cwd=root,
                check=False,
            )
            if result.returncode == 0:
                raise AssertionError(f"{profile} accepted a case-fold collision")

        lfn_source = root / "lfn-invalid"
        lfn_source.mkdir()
        for bad_name in ("trailing.", "trailing ", "bad:name.txt"):
            (lfn_source / bad_name).write_bytes(b"invalid DOS LFN\n")
            result = run(
                boltpack,
                "create",
                "--target-profile=dos-cp850-lfn-v1",
                "dos-lfn-invalid.blt",
                lfn_source.name,
                cwd=root,
                check=False,
            )
            if result.returncode == 0:
                raise AssertionError(f"DOS LFN profile accepted {bad_name!r}")
            (lfn_source / bad_name).unlink()

        long_path = root / "longpath"
        nested = long_path / ("a" * 130) / ("b" * 130)
        nested.mkdir(parents=True)
        (nested / "file.txt").write_bytes(b"path exceeds DOS buffer\n")
        result = run(
            boltpack, "create", "--target-profile=dos-cp850-lfn-v1",
            "dos-lfn-longpath.blt", "longpath", cwd=root, check=False,
        )
        if result.returncode == 0:
            raise AssertionError("DOS LFN profile accepted an overlong path")

    print("DOS-family target profiles integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
