#!/usr/bin/env python3
"""Host integration checks for the Linux creator's Atari target profile."""

import argparse
import pathlib
import subprocess
import tempfile


def escaped_path(value: str) -> str:
    out = []
    for byte in value.encode("utf-8"):
        if 0x20 <= byte <= 0x7E and byte != ord("\\"):
            out.append(chr(byte))
        elif byte == ord("\\"):
            out.append("\\\\")
        else:
            out.append(f"\\x{byte:02X}")
    return "".join(out)


def first_generated_alias(component: str) -> str:
    stem, dot, extension = component.rpartition(".")
    if not dot:
        stem, extension = component, ""
    prefix = "".join(c for c in stem if c.isascii() and c.isalnum())[:2]
    prefix = prefix.upper() or "F"
    ext = "".join(c for c in extension if c.isascii() and c.isalnum())[:3]
    value = 2166136261
    for byte in component.encode("utf-8"):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    alias = f"{prefix}~{value & 0xFFFFF:05X}"
    return f"{alias}.{ext.upper()}" if ext else alias


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(args, text=True, capture_output=True)
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

    payloads = {
        "NORMAL.TXT": b"direct name\n",
        "longfilename01.txt": b"number one\n",
        "longfilename02.txt": b"number two\n",
        "öong.txt": b"unicode name\n",
        "Case.txt": b"upper case collision\n",
        "case.txt": b"lower case collision\n",
        "BPNAMES.TXT": b"reserved report collision\n",
        "long_directory_name/child long name.dat": b"nested long name\n",
        "long_directory_name/deeper/normal.bin": b"nested direct name\n",
    }
    # Occupy the first deterministic alias of longfilename01.txt with a
    # directly legal source name. The allocator must probe to another free
    # alias rather than emit a duplicate destination.
    colliding_alias = first_generated_alias("longfilename01.txt")
    payloads[colliding_alias] = b"preexisting alias collision\n"

    with tempfile.TemporaryDirectory(prefix="boltpack-atari-profile-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        source.mkdir()
        for relative, data in payloads.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

        # The Atari ST encoding can represent ö, but classic GEMDOS filenames
        # reject bytes above 0x7f. Keep this isolated from the other invalid
        # names below so the profile's encoding-versus-legality rule is tested.
        unicode_source = root / "unicode-only"
        unicode_source.mkdir()
        (unicode_source / "öong.txt").write_bytes(b"unicode name\n")
        unicode_archive = root / "unicode-strict.blt"
        unicode_strict = run(
            boltpack,
            "create",
            "--target-profile=atari-st",
            str(unicode_archive),
            str(unicode_source),
            check=False,
        )
        if unicode_strict.returncode == 0 or unicode_archive.exists():
            raise AssertionError("strict Atari profile accepted a high-bit GEMDOS name")
        if "outside the Atari GEMDOS 8.3 profile" not in unicode_strict.stderr:
            raise AssertionError(
                "high-bit Atari-encodable name failed for an unexpected reason: "
                + unicode_strict.stderr
            )

        strict_archive = root / "strict.blt"
        strict = run(
            boltpack,
            "create",
            "--target-profile=atari-st",
            str(strict_archive),
            str(source),
            check=False,
        )
        if strict.returncode == 0 or strict_archive.exists():
            raise AssertionError("strict Atari profile accepted invalid names")

        archive = root / "auto.blt"
        run(
            boltpack,
            "create",
            "--target-profile=atari-st",
            "--target-auto-rename",
            str(archive),
            str(source),
        )

        output = root / "out"
        run(boltpack, "extract", "-o", str(output), str(archive))
        report_path = output / "BPNAMES.TXT"
        if not report_path.is_file():
            raise AssertionError("archive did not extract BPNAMES.TXT")

        lines = report_path.read_text("ascii").splitlines()
        if len(lines) < len(payloads) + 2:
            raise AssertionError("name report is missing source paths")
        rows = [line.split("\t", 1) for line in lines[2:]]
        if any(len(row) != 2 for row in rows):
            raise AssertionError("malformed name report row")
        mapping = dict(rows)
        for relative in payloads:
            original = escaped_path(f"{source}/{relative}")
            if original not in mapping:
                raise AssertionError(f"original path missing from report: {relative}")

        numbered = [
            mapping[escaped_path(f"{source}/longfilename01.txt")],
            mapping[escaped_path(f"{source}/longfilename02.txt")],
        ]
        if numbered[0] == numbered[1]:
            raise AssertionError("numbered long names mapped to the same Atari path")
        direct_target = mapping[escaped_path(f"{source}/{colliding_alias}")]
        if direct_target.rsplit("\\", 1)[-1] != colliding_alias:
            raise AssertionError("direct source name did not keep its Atari alias")
        if numbered[0].rsplit("\\", 1)[-1] == colliding_alias:
            raise AssertionError("generated alias did not probe past an occupied path")

        targets = [row[1] for row in rows]
        if len(targets) != len(set(targets)):
            raise AssertionError("report contains duplicate Atari paths")

        # A unique report is not enough: every destination must name the
        # corresponding extracted entry. In particular, a source named
        # BPNAMES.TXT must be aliased because that path belongs to the report.
        for relative, expected_payload in payloads.items():
            original = escaped_path(f"{source}/{relative}")
            target = mapping[original]
            if relative == "BPNAMES.TXT" and target.upper() == "BPNAMES.TXT":
                raise AssertionError("source file occupied the reserved report path")
            # The report records native Atari GEMDOS paths with backslash
            # separators, while the archive stores canonical slash paths.
            extracted = output.joinpath(
                *target.replace("\\", "/").rstrip("/").split("/")
            )
            if not extracted.is_file() or extracted.read_bytes() != expected_payload:
                raise AssertionError(
                    f"reported Atari path does not resolve to the original payload: "
                    f"{relative} -> {target}"
                )

        actual_payloads = [
            path.read_bytes()
            for path in output.rglob("*")
            if path.is_file() and path != report_path
        ]
        if sorted(actual_payloads) != sorted(payloads.values()):
            raise AssertionError("payloads changed or one or more files were omitted")

    print("Atari target profile auto-rename integration: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
