#!/usr/bin/env python3
"""Test the Windows NT target profile through Wine and Linux-made fixtures."""

import argparse
import pathlib
import struct
import subprocess
import tempfile


def run_windows(binary: str, wine: str, *args: str, cwd: pathlib.Path,
                check: bool = True):
    result = subprocess.run(
        [wine, binary, *args], cwd=cwd, text=True, errors="replace",
        capture_output=True, timeout=90,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"Windows command failed ({result.returncode}): {' '.join(args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def run_linux(binary: str, *args: str, cwd: pathlib.Path, check: bool = True):
    result = subprocess.run(
        [binary, *args], cwd=cwd, text=True, errors="replace",
        capture_output=True,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"Linux command failed ({result.returncode}): {' '.join(args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def make_fixture(linux: str, root: pathlib.Path, directory: str,
                 archive: str, names: tuple[str, ...]):
    source = root / directory
    source.mkdir()
    for index, name in enumerate(names):
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(f"fixture {index}\n".encode())
    run_linux(linux, "create", archive, directory, cwd=root)
    return root / archive


def archive_entries(path: pathlib.Path):
    data = path.read_bytes()
    table = struct.unpack_from("<Q", data, len(data) - 20)[0]
    count = struct.unpack_from("<I", data, table + 4)[0]
    record = table + 16
    entries = []
    for _ in range(count):
        name_len = struct.unpack_from("<H", data, record)[0]
        name = data[record + 2:record + 2 + name_len].decode("utf-8")
        fields = record + 2 + name_len
        entries.append((name, data[fields + 16]))
        record = fields + 43
    return entries


def check_rejected_append(binary: str, wine: str, archive: pathlib.Path,
                          root: pathlib.Path, new_file: str, label: str):
    before = archive.read_bytes()
    result = run_windows(
        binary, wine, "add", "--target-profile=windows-nt-lfn-v1",
        archive.name, new_file, cwd=root, check=False,
    )
    if result.returncode == 0 or archive.read_bytes() != before:
        raise AssertionError(f"Windows profile accepted {label} or changed archive")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--linux-cli", required=True)
    parser.add_argument("--wine", default="wine64")
    args = parser.parse_args()
    binary = str(pathlib.Path(args.binary).resolve())
    linux = str(pathlib.Path(args.linux_cli).resolve())

    with tempfile.TemporaryDirectory(prefix="boltpack-windows-target-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        source.mkdir()
        expected = {
            "Résumé.txt": b"UTF-8 Windows path\n",
            "資料/nested.bin": bytes(range(256)) * 7,
        }
        for relative, payload in expected.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        run_windows(
            binary, args.wine, "create", "--target-profile=windows-nt-lfn-v1",
            "valid.blt", "source", cwd=root,
        )
        run_windows(binary, args.wine, "test", "valid.blt", cwd=root)
        run_linux(linux, "test", "valid.blt", cwd=root)

        added = source / "added.txt"
        added.write_bytes(b"appended\n")
        run_windows(
            binary, args.wine, "add", "--target-profile=windows-nt-lfn-v1",
            "valid.blt", "source/added.txt", cwd=root,
        )
        run_windows(binary, args.wine, "test", "valid.blt", cwd=root)
        output = root / "out"
        run_windows(binary, args.wine, "extract", "-o", "out", "valid.blt",
                    cwd=root)
        for name, payload in expected.items():
            if (output / "source" / name).read_bytes() != payload:
                raise AssertionError(f"Windows round trip changed {name!r}")
        if (output / "source" / "added.txt").read_bytes() != b"appended\n":
            raise AssertionError("Windows append round trip changed added payload")

        valid_add = source / "valid-add.txt"
        valid_add.write_bytes(b"valid add\n")
        for fixture_id, (label, names) in enumerate((
            ("case-fold collision", ("Foo.txt", "foo.TXT")),
            ("Unicode normalization collision", ("\u00e9.txt", "e\u0301.txt")),
            ("reserved device name", ("CON.txt",)),
            ("reserved superscript device name", ("COM\u00b2.txt",)),
            ("trailing dot", ("trailing.",)),
            ("trailing space", ("trailing ",)),
            ("overlong relative path", ("a" * 130 + "/" + "b" * 130,)),
        )):
            fixture = f"fixture-{fixture_id}"
            archive_name = f"invalid-{fixture_id}.blt"
            archive = make_fixture(
                linux, root, fixture, archive_name, names,
            )
            check_rejected_append(binary, args.wine, archive, root,
                                  "source/valid-add.txt", label)

        # A Windows creator can also target Classic Mac HFS. The profile
        # omits Windows FAT metadata, but preserves canonical UTF-8 names and
        # validates their exact MacRoman projection.
        hfs_dir = root / "hfs-source"
        hfs_dir.mkdir()
        hfs_name = "classic-é.txt"
        (hfs_dir / hfs_name).write_bytes(b"Classic HFS from Windows\n")
        hfs_archive = root / "classic-hfs.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=macos-classic-hfs-v1",
                    hfs_archive.name, hfs_dir.name, cwd=root)
        if archive_entries(hfs_archive) != [
                ("hfs-source/", 4), ("hfs-source/" + hfs_name, 0)]:
            raise AssertionError("Windows HFS archive contains unexpected metadata or names")
        run_linux(linux, "test", hfs_archive.name, cwd=root)
        hfs_archive_before = hfs_archive.read_bytes()
        (hfs_dir / "CLASSIC-É.TXT").write_bytes(b"MacRoman alias\n")
        result = run_windows(
            binary, args.wine, "add",
            "--target-profile=macos-classic-hfs-v1", hfs_archive.name,
            "hfs-source/CLASSIC-É.TXT", cwd=root, check=False)
        if result.returncode == 0 or hfs_archive.read_bytes() != hfs_archive_before:
            raise AssertionError("Windows HFS profile accepted a MacRoman collision")
        (hfs_dir / "not-macroman-漢.txt").write_bytes(b"unrepresentable\n")
        result = run_windows(
            binary, args.wine, "create",
            "--target-profile=macos-classic-hfs-v1", "hfs-bad-name.blt",
            "hfs-source/not-macroman-漢.txt", cwd=root, check=False)
        if result.returncode == 0 or (root / "hfs-bad-name.blt").exists():
            raise AssertionError("Windows HFS profile accepted an unmappable name")
        readonly = hfs_dir / "readonly.txt"
        readonly.write_bytes(b"readonly source\n")
        readonly.chmod(0o444)
        result = run_windows(
            binary, args.wine, "create",
            "--target-profile=macos-classic-hfs-v1", "hfs-readonly.blt",
            "hfs-source/readonly.txt", cwd=root, check=False)
        readonly.chmod(0o644)
        if result.returncode == 0 or (root / "hfs-readonly.blt").exists():
            raise AssertionError("Windows HFS profile accepted a read-only source")
        result = run_windows(binary, args.wine, "create",
                             "--target-profile=macos-classic-hfs-v1", "-F",
                             "hfs-bad-codec.blt", "hfs-source/" + hfs_name,
                             cwd=root, check=False)
        if result.returncode == 0 or (root / "hfs-bad-codec.blt").exists():
            raise AssertionError("Windows HFS profile accepted Fluff")

        ordinary_windows_archive = root / "windows-with-fat-meta.blt"
        run_windows(binary, args.wine, "create", ordinary_windows_archive.name,
                    "hfs-source/" + hfs_name, cwd=root)
        ordinary_before = ordinary_windows_archive.read_bytes()
        result = run_windows(
            binary, args.wine, "add",
            "--target-profile=macos-classic-hfs-v1",
            ordinary_windows_archive.name, "source/valid-add.txt", cwd=root,
            check=False)
        if result.returncode == 0 or ordinary_windows_archive.read_bytes() != ordinary_before:
            raise AssertionError("Windows HFS profile accepted existing FAT metadata")

        # The Windows creator can also produce the conservative AmigaOS 68k
        # profile, projecting UTF-8 source paths exactly through Latin-1.
        amiga_dir = root / "amiga-source"
        amiga_dir.mkdir()
        amiga_name = "Grüße-café.txt"
        (amiga_dir / amiga_name).write_bytes(b"Amiga Latin-1 from Windows\n")
        amiga_archive = root / "amiga.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=amigaos-68k-latin1",
                    amiga_archive.name, amiga_dir.name, cwd=root)
        amiga_entries = archive_entries(amiga_archive)
        if amiga_entries != [
                ("amiga-source/", 4),
                ("amiga-source/" + amiga_name, 0)]:
            raise AssertionError(
                "Windows Amiga archive contains unexpected metadata or names")
        run_windows(binary, args.wine, "test", amiga_archive.name, cwd=root)
        run_linux(linux, "test", amiga_archive.name, cwd=root)

        amiga_added = amiga_dir / "added.txt"
        amiga_added.write_bytes(b"appended under Amiga profile\n")
        run_windows(binary, args.wine, "add",
                    "--target-profile=amigaos-68k-latin1",
                    amiga_archive.name, "amiga-source/added.txt", cwd=root)
        run_windows(binary, args.wine, "test", amiga_archive.name, cwd=root)
        run_linux(linux, "test", amiga_archive.name, cwd=root)
        if archive_entries(amiga_archive) != [
                ("amiga-source/", 4),
                ("amiga-source/" + amiga_name, 0),
                ("amiga-source/added.txt", 0)]:
            raise AssertionError("Windows Amiga append changed names or added metadata")

        amiga_before = amiga_archive.read_bytes()
        alias_name = "GRÜßE-CAFÉ.TXT"
        (amiga_dir / alias_name).write_bytes(b"Latin-1 case alias\n")
        result = run_windows(
            binary, args.wine, "add",
            "--target-profile=amigaos-68k-latin1", amiga_archive.name,
            "amiga-source/" + alias_name, cwd=root, check=False)
        if result.returncode == 0 or amiga_archive.read_bytes() != amiga_before:
            raise AssertionError("Windows Amiga profile accepted a Latin-1 case alias")

        (amiga_dir / "unmappable-漢.txt").write_bytes(b"unmappable\n")
        result = run_windows(
            binary, args.wine, "create",
            "--target-profile=amigaos-68k-latin1", "amiga-bad-name.blt",
            "amiga-source/unmappable-漢.txt", cwd=root, check=False)
        if result.returncode == 0 or (root / "amiga-bad-name.blt").exists():
            raise AssertionError("Windows Amiga profile accepted an unmappable name")

        read_only = amiga_dir / "readonly.txt"
        read_only.write_bytes(b"read-only source\n")
        read_only.chmod(0o444)
        result = run_windows(
            binary, args.wine, "create",
            "--target-profile=amigaos-68k-latin1", "amiga-readonly.blt",
            "amiga-source/readonly.txt", cwd=root, check=False)
        read_only.chmod(0o644)
        if result.returncode == 0 or (root / "amiga-readonly.blt").exists():
            raise AssertionError("Windows Amiga profile accepted a read-only source")

        for bad_args, label in ((
                ("-F",), "Fluff"),
                (("-b", "32k"), "32 KiB blocks")):
            result = run_windows(
                binary, args.wine, "create", *bad_args,
                "--target-profile=amigaos-68k-latin1",
                f"amiga-bad-{label.replace(' ', '-')}.blt",
                "amiga-source/" + amiga_name, cwd=root, check=False)
            if result.returncode == 0:
                raise AssertionError(f"Windows Amiga profile accepted {label}")

        amiga_fat_archive = root / "amiga-existing-fat-meta.blt"
        run_windows(binary, args.wine, "create", amiga_fat_archive.name,
                    "amiga-source/" + amiga_name, cwd=root)
        amiga_fat_before = amiga_fat_archive.read_bytes()
        result = run_windows(
            binary, args.wine, "add",
            "--target-profile=amigaos-68k-latin1",
            amiga_fat_archive.name, "source/valid-add.txt", cwd=root,
            check=False)
        if result.returncode == 0 or amiga_fat_archive.read_bytes() != amiga_fat_before:
            raise AssertionError(
                "Windows Amiga profile accepted existing portable FAT metadata")

        # DOS 8.3 profiles reuse the Windows UTF-8 source paths but require an
        # exact target codepage projection and DOS filename legality.
        dos_dir = root / "DOSSRC"
        dos_dir.mkdir()
        (dos_dir / "café.txt").write_bytes(b"CP850 name\n")
        (dos_dir / "subdir").mkdir()
        (dos_dir / "subdir" / "DATA.BIN").write_bytes(b"nested 8.3\n")
        dos850_archive = root / "dos850.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=dos-cp850-83",
                    dos850_archive.name, dos_dir.name, cwd=root)
        run_windows(binary, args.wine, "test", dos850_archive.name, cwd=root)
        run_linux(linux, "test", dos850_archive.name, cwd=root)
        dos_primary = [
            (name, entry_type) for name, entry_type in archive_entries(dos850_archive)
            if entry_type in (0, 4)
        ]
        if dos_primary != [
                ("DOSSRC/", 4), ("DOSSRC/café.txt", 0),
                ("DOSSRC/subdir/", 4),
                ("DOSSRC/subdir/DATA.BIN", 0)]:
            raise AssertionError("Windows DOS CP850 profile changed names or types")
        if not any(entry_type == 2
                   for _, entry_type in archive_entries(dos850_archive)):
            raise AssertionError("Windows DOS profile omitted portable FAT metadata")

        (dos_dir / "added.txt").write_bytes(b"DOS profile append\n")
        run_windows(binary, args.wine, "add",
                    "--target-profile=dos-cp850-83", dos850_archive.name,
                    "DOSSRC/added.txt", cwd=root)
        run_linux(linux, "test", dos850_archive.name, cwd=root)

        dos437_dir = root / "D437SRC"
        dos437_dir.mkdir()
        (dos437_dir / "über.txt").write_bytes(b"CP437 name\n")
        dos437_archive = root / "dos437.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=dos-cp437-83", dos437_archive.name,
                    dos437_dir.name, cwd=root)
        run_windows(binary, args.wine, "test", dos437_archive.name, cwd=root)
        run_linux(linux, "test", dos437_archive.name, cwd=root)
        dos437_primary = [
            (name, entry_type) for name, entry_type in archive_entries(dos437_archive)
            if entry_type in (0, 4)
        ]
        if dos437_primary != [("D437SRC/", 4), ("D437SRC/über.txt", 0)]:
            raise AssertionError("Windows DOS CP437 profile changed the projected name")

        for profile_name, source_name in (
                ("dos16-cp850-mini", "DOSSRC"),
                ("win16-cp437-mini", "D437SRC"),
                ("os2-16-cp850-mini", "DOSSRC"),
                ("os2-fat-cp850", "DOSSRC")):
            profile_archive = f"{profile_name}.blt"
            run_windows(
                binary, args.wine, "create",
                f"--target-profile={profile_name}", profile_archive,
                source_name, cwd=root)
            run_linux(linux, "test", profile_archive, cwd=root)

        lfn_name = "Long Filename Ø With Spaces.txt"
        (root / lfn_name).write_bytes(b"DOS LFN profile payload\n")
        lfn_archive = root / "dos-cp850-lfn.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=dos-cp850-lfn-v1",
                    lfn_archive.name, lfn_name, cwd=root)
        lfn_primary = [
            item for item in archive_entries(lfn_archive)
            if item[1] in (0, 4)
        ]
        if lfn_primary != [(lfn_name, 0)]:
            raise AssertionError("Windows DOS LFN profile changed the CP850 name")
        run_windows(binary, args.wine, "test", lfn_archive.name, cwd=root)
        run_linux(linux, "test", lfn_archive.name, cwd=root)
        lfn_before = lfn_archive.read_bytes()
        lfn_alias = "long filename ø with spaces.TXT"
        (root / lfn_alias).write_bytes(b"case alias\n")
        result = run_windows(
            binary, args.wine, "add", "--target-profile=dos-cp850-lfn-v1",
            lfn_archive.name, lfn_alias, cwd=root, check=False)
        if result.returncode == 0 or lfn_archive.read_bytes() != lfn_before:
            raise AssertionError("Windows DOS LFN profile accepted a case alias")
        for invalid_index, invalid_name in enumerate((
                "CON.txt", "bad?.txt", "trailing.", "trailing ",
                "unmappable-漢.txt")):
            (root / invalid_name).write_bytes(b"invalid DOS name\n")
            bad_archive = f"dos-lfn-invalid-{invalid_index}.blt"
            result = run_windows(
                binary, args.wine, "create",
                "--target-profile=dos-cp850-lfn-v1", bad_archive,
                invalid_name, cwd=root, check=False)
            if result.returncode == 0 or (root / bad_archive).exists():
                raise AssertionError(
                    f"Windows DOS LFN profile accepted {invalid_name!r}")
        result = run_windows(
            binary, args.wine, "create", "-F",
            "--target-profile=dos-cp850-lfn-v1", "dos-lfn-fluff.blt",
            lfn_name, cwd=root, check=False)
        if result.returncode == 0:
            raise AssertionError("Windows DOS LFN profile accepted Fluff")

        (dos_dir / "unmappable-漢.txt").write_bytes(b"unmappable\n")
        result = run_windows(
            binary, args.wine, "create",
            "--target-profile=dos-cp850-83", "dos-unmappable.blt",
            "DOSSRC/unmappable-漢.txt", cwd=root, check=False)
        if result.returncode == 0 or (root / "dos-unmappable.blt").exists():
            raise AssertionError("Windows DOS profile accepted an unmappable name")

        for fixture_id, names in enumerate((
                ("TOOLONG99.TXT",), ("CON.TXT",),
                ("café.txt", "CAFÉ.TXT"))):
            fixture_name = f"dos-invalid-{fixture_id}"
            invalid_archive = make_fixture(
                linux, root, fixture_name,
                f"dos-invalid-{fixture_id}.blt", names)
            before = invalid_archive.read_bytes()
            result = run_windows(
                binary, args.wine, "add",
                "--target-profile=dos-cp850-83", invalid_archive.name,
                "dos-source/added.txt", cwd=root, check=False)
            if result.returncode == 0 or invalid_archive.read_bytes() != before:
                raise AssertionError("Windows DOS 8.3 profile accepted invalid names")

        for profile_name, archive_name in (
                ("dos-cp850-83", "dos850-bad-codec.blt"),
                ("dos-cp437-83", "dos437-bad-codec.blt")):
            result = run_windows(
                binary, args.wine, "create", "-F",
                f"--target-profile={profile_name}", archive_name,
                dos_dir.name, cwd=root, check=False)
            if result.returncode == 0:
                raise AssertionError(f"Windows {profile_name} accepted Fluff")

        # CP/M-86 uses a flat uppercase ASCII 8.3 namespace. The Windows
        # writer projects lowercase names and omits portable FAT attributes.
        (root / "mixed.txt").write_bytes(b"CP/M-86 profile payload\n")
        cpm_archive = root / "cpm86.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=cpm86-83", cpm_archive.name,
                    "mixed.txt", cwd=root)
        if archive_entries(cpm_archive) != [("MIXED.TXT", 0)]:
            raise AssertionError("Windows CP/M-86 archive has wrong name or metadata")
        run_windows(binary, args.wine, "test", cpm_archive.name, cwd=root)
        run_linux(linux, "test", cpm_archive.name, cwd=root)
        (root / "added.txt").write_bytes(b"append\n")
        run_windows(binary, args.wine, "add", "--target-profile=cpm86-83",
                    cpm_archive.name, "added.txt", cwd=root)
        if archive_entries(cpm_archive) != [("MIXED.TXT", 0), ("ADDED.TXT", 0)]:
            raise AssertionError("Windows CP/M-86 append did not project names")
        run_linux(linux, "test", cpm_archive.name, cwd=root)
        cpm_before = cpm_archive.read_bytes()
        (root / "MIXED.TXT").write_bytes(b"collision\n")
        result = run_windows(
            binary, args.wine, "add", "--target-profile=cpm86-83",
            cpm_archive.name, "MIXED.TXT", cwd=root, check=False)
        if result.returncode == 0 or cpm_archive.read_bytes() != cpm_before:
            raise AssertionError("Windows CP/M-86 accepted a projected collision")
        (root / "CPMDIR").mkdir()
        (root / "CPMDIR" / "NESTED.TXT").write_bytes(b"nested\n")
        result = run_windows(
            binary, args.wine, "create", "--target-profile=cpm86-83",
            "cpm-directory.blt", "CPMDIR", cwd=root, check=False)
        if result.returncode == 0 or (root / "cpm-directory.blt").exists():
            raise AssertionError("Windows CP/M-86 accepted directory entries")
        result = run_windows(
            binary, args.wine, "create", "-F", "--target-profile=cpm86-83",
            "cpm-fluff.blt", "mixed.txt", cwd=root, check=False)
        if result.returncode == 0 or (root / "cpm-fluff.blt").exists():
            raise AssertionError("Windows CP/M-86 accepted Fluff")
        ordinary_cpm_archive = root / "cpm-existing-fat-meta.blt"
        run_windows(binary, args.wine, "create", ordinary_cpm_archive.name,
                    "mixed.txt", cwd=root)
        cpm_metadata_before = ordinary_cpm_archive.read_bytes()
        result = run_windows(
            binary, args.wine, "add", "--target-profile=cpm86-83",
            ordinary_cpm_archive.name, "added.txt", cwd=root, check=False)
        if result.returncode == 0 or ordinary_cpm_archive.read_bytes() != cpm_metadata_before:
            raise AssertionError("Windows CP/M-86 accepted existing FAT metadata")

        # Atari GEMDOS uses a printable ASCII, uppercase 8.3 namespace and
        # accepts portable FAT metadata. The strict writer profile must keep
        # payloads intact while refusing lossy Unicode names and aliases.
        atari_source = root / "MiXeD.TXT"
        atari_source.write_bytes(b"Atari target profile payload\n")
        atari_archive = root / "atari-st.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=atari-st", atari_archive.name,
                    atari_source.name, cwd=root)
        atari_data = [entry for entry in archive_entries(atari_archive)
                      if entry[1] in (0, 1)]
        if atari_data != [("MiXeD.TXT", 0)]:
            raise AssertionError("Windows Atari profile changed the canonical archive name")
        run_windows(binary, args.wine, "test", atari_archive.name, cwd=root)
        run_linux(linux, "test", atari_archive.name, cwd=root)
        atari_before = atari_archive.read_bytes()
        atari_alias = root / "mixed.txt"
        atari_alias.write_bytes(b"collision\n")
        result = run_windows(
            binary, args.wine, "add", "--target-profile=atari-st",
            atari_archive.name, atari_alias.name, cwd=root, check=False)
        if result.returncode == 0 or atari_archive.read_bytes() != atari_before:
            raise AssertionError("Windows Atari profile accepted case alias")

        non_ascii = root / "öong.txt"
        non_ascii.write_bytes(b"not GEMDOS legal\n")
        result = run_windows(
            binary, args.wine, "create", "--target-profile=atari-st",
            "atari-nonascii.blt", non_ascii.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "atari-nonascii.blt").exists():
            raise AssertionError("Windows Atari profile accepted a high-bit name")

        result = run_windows(
            binary, args.wine, "create", "-F", "--target-profile=atari-st",
            "atari-fluff.blt", atari_source.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "atari-fluff.blt").exists():
            raise AssertionError("Windows Atari profile accepted Fluff")

        # QDOS maps archive separators to underscores, so an ordinary file
        # name can alias a nested path even when the host filesystem allows
        # both. Latin-1 names survive; foreign metadata and Fluff do not.
        ql_source = root / "café.txt"
        ql_source.write_bytes(b"Sinclair QL payload\n")
        ql_archive = root / "sinclair-ql.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=sinclair-ql-latin1-v1",
                    ql_archive.name, ql_source.name, cwd=root)
        ql_entries = archive_entries(ql_archive)
        if ql_entries != [("café.txt", 0)]:
            raise AssertionError("Windows QL profile did not omit foreign metadata")
        run_linux(linux, "test", ql_archive.name, cwd=root)
        ql_append = run_windows(
            binary, args.wine, "add",
            "--target-profile=sinclair-ql-latin1-v1",
            ql_archive.name, "café.txt", cwd=root, check=False)
        # The append above must refuse an existing projected-name collision.
        if ql_append.returncode == 0 or archive_entries(ql_archive) != ql_entries:
            raise AssertionError("Windows QL profile accepted an append collision")

        ql_tree = root / "ql-tree"
        ql_tree.mkdir()
        (ql_tree / "a_b.txt").write_bytes(b"flat\n")
        (ql_tree / "a").mkdir()
        (ql_tree / "a" / "b.txt").write_bytes(b"nested\n")
        result = run_windows(
            binary, args.wine, "create",
            "--target-profile=sinclair-ql-latin1-v1", "ql-collision.blt",
            ql_tree.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "ql-collision.blt").exists():
            raise AssertionError("Windows QL profile missed slash-to-underscore collision")

        ql_unmappable = root / "ql-🚀.txt"
        ql_unmappable.write_bytes(b"not Latin-1\n")
        result = run_windows(
            binary, args.wine, "create",
            "--target-profile=sinclair-ql-latin1-v1", "ql-unmappable.blt",
            ql_unmappable.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "ql-unmappable.blt").exists():
            raise AssertionError("Windows QL profile accepted an unmappable name")

        result = run_windows(
            binary, args.wine, "create", "-F",
            "--target-profile=sinclair-ql-latin1-v1", "ql-fluff.blt",
            ql_source.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "ql-fluff.blt").exists():
            raise AssertionError("Windows QL profile accepted Fluff")

        # RISC OS FileCore uses ten-byte Latin-1 leaves and reserves dot as
        # directory syntax. The profile stores no portable Windows metadata.
        riscos_source = root / "café"
        riscos_source.write_bytes(b"RISC OS FileCore payload\n")
        riscos_archive = root / "riscos.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=riscos-latin1-v1",
                    riscos_archive.name, riscos_source.name, cwd=root)
        riscos_entries = archive_entries(riscos_archive)
        if riscos_entries != [("café", 0)]:
            raise AssertionError("Windows RISC OS profile retained foreign metadata")
        run_linux(linux, "test", riscos_archive.name, cwd=root)
        riscos_append = run_windows(
            binary, args.wine, "add", "--target-profile=riscos-latin1-v1",
            riscos_archive.name, riscos_source.name, cwd=root, check=False)
        if riscos_append.returncode == 0 or archive_entries(riscos_archive) != riscos_entries:
            raise AssertionError("Windows RISC OS profile accepted append collision")

        riscos_long = root / "12345678901"
        riscos_long.write_bytes(b"too long\n")
        result = run_windows(
            binary, args.wine, "create", "--target-profile=riscos-latin1-v1",
            "riscos-long.blt", riscos_long.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "riscos-long.blt").exists():
            raise AssertionError("Windows RISC OS profile accepted 11-byte component")

        riscos_dotted = root / "bad.name"
        riscos_dotted.write_bytes(b"dot is path syntax\n")
        result = run_windows(
            binary, args.wine, "create", "--target-profile=riscos-latin1-v1",
            "riscos-dotted.blt", riscos_dotted.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "riscos-dotted.blt").exists():
            raise AssertionError("Windows RISC OS profile accepted a dot in a leaf")

        riscos_alias_archive = make_fixture(
            linux, root, "riscos-case-alias", "riscos-case-alias.blt",
            ("É", "é"))
        (root / "new-file").write_bytes(b"append\n")
        alias_before = riscos_alias_archive.read_bytes()
        result = run_windows(
            binary, args.wine, "add", "--target-profile=riscos-latin1-v1",
            riscos_alias_archive.name, "new-file", cwd=root, check=False)
        if result.returncode == 0 or riscos_alias_archive.read_bytes() != alias_before:
            raise AssertionError("Windows RISC OS profile accepted existing case aliases")

        result = run_windows(
            binary, args.wine, "create", "-F",
            "--target-profile=riscos-latin1-v1", "riscos-fluff.blt",
            riscos_source.name, cwd=root, check=False)
        if result.returncode == 0 or (root / "riscos-fluff.blt").exists():
            raise AssertionError("Windows RISC OS profile accepted Fluff")

        # Human68k uses standard Shift-JIS 18.3 names, and a 0x5c trail byte
        # must remain data instead of becoming a path separator.
        x68_source = root / "x68-source"
        x68_source.mkdir()
        x68_expected = {
            "plain": b"ASCII Human68k name\n",
            "日本語.bin": b"standard Shift-JIS name\n",
            "trail-―.bin": b"trail byte 0x5c stays in character\n",
            "123456789012345678.xyz": b"18.3 boundary\n",
        }
        for name, payload in x68_expected.items():
            (x68_source / name).write_bytes(payload)
        x68_archive = root / "x68000.blt"
        run_windows(binary, args.wine, "create",
                    "--target-profile=x68000-sjis-v1", x68_archive.name,
                    x68_source.name, cwd=root)
        x68_entries = archive_entries(x68_archive)
        if any(entry[1] == 2 for entry in x68_entries):
            raise AssertionError("Windows X68000 profile retained foreign metadata")
        run_windows(binary, args.wine, "test", x68_archive.name, cwd=root)
        run_linux(linux, "test", x68_archive.name, cwd=root)

        for label, name in (
                ("stem", "A" * 19), ("extension", "file.long"),
                ("multiple-dots", "two.dots.ext"),
                ("unmappable", "bad-🚀")):
            invalid_dir = root / f"x68bad{label}"
            invalid_dir.mkdir()
            (invalid_dir / name).write_bytes(b"profile must reject this name\n")
            result = run_windows(
                binary, args.wine, "create",
                "--target-profile=x68000-sjis-v1", f"x68-{label}.blt",
                invalid_dir.name, cwd=root, check=False)
            if result.returncode == 0 or (root / f"x68-{label}.blt").exists():
                raise AssertionError(f"Windows X68000 profile accepted {label}")

        x68_case_alias = make_fixture(
            linux, root, "x68-case-alias", "x68-case-alias.blt",
            ("Readme.txt", "README.TXT"))
        (root / "x68-added").write_bytes(b"append\n")
        x68_before = x68_case_alias.read_bytes()
        result = run_windows(
            binary, args.wine, "add", "--target-profile=x68000-sjis-v1",
            x68_case_alias.name, "x68-added", cwd=root, check=False)
        if result.returncode == 0 or x68_case_alias.read_bytes() != x68_before:
            raise AssertionError("Windows X68000 profile accepted case aliases")

        (root / "x68-node").write_bytes(b"file blocks child path\n")
        run_linux(linux, "create", "x68-prefix.blt", "x68-node", cwd=root)
        (root / "x68-node").unlink()
        (root / "x68-node").mkdir()
        (root / "x68-node" / "child.bin").write_bytes(b"child\n")
        x68_prefix_before = (root / "x68-prefix.blt").read_bytes()
        result = run_windows(
            binary, args.wine, "add", "--target-profile=x68000-sjis-v1",
            "x68-prefix.blt", "x68-node/child.bin", cwd=root, check=False)
        if result.returncode == 0 or (root / "x68-prefix.blt").read_bytes() != x68_prefix_before:
            raise AssertionError("Windows X68000 profile accepted file/directory prefix conflict")

        # The profile path ceiling is independent of component limits. Build
        # an IP1 archive with individually valid 18-byte components whose
        # combined archive member path exceeds 255 Shift-JIS bytes.
        x68_long_root = root / "x68-long"
        x68_deep = x68_long_root
        for _ in range(14):
            x68_deep = x68_deep / "ABCDEFGHIJKLMNOPQR"
        x68_deep.mkdir(parents=True)
        (x68_deep / "f").write_bytes(b"overlong projected path\n")
        run_linux(linux, "create", "--interchange-profile-1", "-1",
                  "x68-overlong-path.blt", x68_long_root.name, cwd=root)
        x68_path_before = (root / "x68-overlong-path.blt").read_bytes()
        result = run_windows(
            binary, args.wine, "add", "--target-profile=x68000-sjis-v1",
            "x68-overlong-path.blt", "x68-added", cwd=root, check=False)
        if result.returncode == 0 or (root / "x68-overlong-path.blt").read_bytes() != x68_path_before:
            raise AssertionError("Windows X68000 profile accepted a path over 255 bytes")

        for options in (("-F",), ("-b", "32k"), ("-F",)):
            result = run_windows(
                binary, args.wine, "create", *options,
                "--target-profile=x68000-sjis-v1", "x68-bad-limit.blt",
                x68_source.name, cwd=root, check=False)
            if result.returncode == 0 or (root / "x68-bad-limit.blt").exists():
                raise AssertionError(f"Windows X68000 profile accepted {options}")

    print("Windows NT LFN, Classic HFS, AmigaOS, Atari ST, Sinclair QL, RISC OS, X68000, DOS 8.3/mini and CP/M-86 profiles: PASS (Wine)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
