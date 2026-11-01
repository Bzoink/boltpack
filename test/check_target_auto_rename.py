#!/usr/bin/env python3
"""Exercise target-specific aliases, report encoding and collisions."""

import argparse
import pathlib
import re
import subprocess
import tempfile


def run(exe, root, *args, allow_partial=False):
    result = subprocess.run([exe, *args], cwd=root, capture_output=True)
    if result.returncode and not (allow_partial and result.returncode == 1 and
                                  b"some entries or attributes were not restored" in result.stderr):
        raise AssertionError(f"{args}: {result.stderr.decode(errors='replace')}")


def report_key(name):
    return "".join(chr(c) if 32 <= c <= 126 and c != 92 else
                   "\\\\" if c == 92 else "\\x%02X" % c
                   for c in name.encode("utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    parser.add_argument("--reader", help="alternate CLI used to extract the archives")
    args = parser.parse_args()
    exe = str(pathlib.Path(args.boltpack).resolve())
    reader = str(pathlib.Path(args.reader).resolve()) if args.reader else exe
    for profile, codepage in (("dos-cp850-83", "cp850"),
                              ("dos-cp437-83", "cp437"),
                              ("dos16-cp850-mini", "cp850"),
                              ("win16-cp437-mini", "cp437"),
                              ("os2-16-cp850-mini", "cp850"),
                              ("os2-fat-cp850", "cp850"),
                              ("dos-cp850-lfn-v1", "cp850"),
                              ("amigaos-68k-latin1", "latin1"),
                              ("macos-classic-hfs-v1", "mac_roman"),
                              ("sinclair-ql-latin1-v1", "latin1"),
                              ("riscos-latin1-v1", "latin1"),
                              ("x68000-sjis-v1", "shift_jis"),
                              ("linux-posix-v1", "utf8"),
                              ("linux-mini-v1", "utf8"),
                              ("posix-utf8-v1", "utf8"),
                              ("plan9-utf8-v1", "utf8"),
                              ("haiku-bfs-v1", "utf8"),
                              ("cpm86-83", "utf8")):
        print(profile, flush=True)
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            files = {"café.txt": b"accent", "CAFÉ.TXT": b"accent upper",
                     "café": b"accent no extension", "BPNAMESTXT": b"report collision riscos",
                     "日本語.dat": b"japanese",
                     "ソ.dat": b"shift-jis trail byte",
                     "ABCDEFGHIJKLMNOPQR.TXT": b"human68k limit",
                     "ABCDEFGHIJKLMNOPQRS.TXT": b"human68k over limit",
                     "N" * 30: b"amiga limit", "M" * 31: b"hfs limit",
                     "é" * 35 + ".txt": b"long latin1 name",
                     "日本" * 10 + ".dat": b"long japanese name",
                     "control\tfile.txt": b"control character",
                     "nested folder/child.txt": b"nested", "CASE.TXT": b"upper",
                     "mix_a.txt": b"flat", "mix/a.txt": b"tree",
                     "case.txt": b"lower", "BPNAMES.TXT": b"report collision",
                     "long-name-1.txt": b"first", "long-name-2.txt": b"second",
                     "unmappable-🧪.txt": b"unicode"}
            for name, contents in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(contents)
            run(exe, root, "create", "--target-profile=" + profile,
                "--target-auto-rename", "archive.blt", *files)
            if profile == "plan9-utf8-v1":
                (root / "append.txt").write_bytes(b"appended")
                run(exe, root, "add", "--target-profile=" + profile,
                    "archive.blt", "append.txt")
            run(reader, root, "extract", "-o", "out", "archive.blt",
                allow_partial=profile == "plan9-utf8-v1")
            report_name = "BPNAMESTXT" if profile == "riscos-latin1-v1" else "BPNAMES.TXT"
            report = (root / "out" / report_name).read_bytes().decode(codepage)
            rows = dict(line.split("\t", 1) for line in report.splitlines()[2:])
            modern = profile in ("linux-posix-v1", "linux-mini-v1", "posix-utf8-v1",
                                 "plan9-utf8-v1", "haiku-bfs-v1")
            if profile == "riscos-latin1-v1":
                assert rows[report_key("café")] == "café"
                assert rows["BPNAMESTXT"].upper() != report_name
            elif profile not in ("cpm86-83", "x68000-sjis-v1"):
                assert {rows[report_key("café.txt")], rows[report_key("CAFÉ.TXT")]} & {"café.txt", "CAFÉ.TXT"}
            keys = list(rows.values()) if modern else [name.upper() for name in rows.values()]
            assert len(set(keys)) == len(rows)
            assert rows["BPNAMES.TXT"].upper() != "BPNAMES.TXT"
            if profile in ("dos-cp850-lfn-v1", "amigaos-68k-latin1",
                           "macos-classic-hfs-v1", "sinclair-ql-latin1-v1",
                           "x68000-sjis-v1") or modern:
                assert rows["long-name-1.txt"] == "long-name-1.txt"
                if modern:
                    assert rows[report_key("unmappable-🧪.txt")] == "unmappable-🧪.txt"
                else:
                    assert len(rows[report_key("unmappable-🧪.txt")]) > 12
            if profile == "x68000-sjis-v1":
                assert rows[report_key("日本語.dat")] == "日本語.dat"
                assert rows[report_key("ソ.dat")] == "ソ.dat"
                assert rows["ABCDEFGHIJKLMNOPQR.TXT"] == "ABCDEFGHIJKLMNOPQR.TXT"
                assert rows["ABCDEFGHIJKLMNOPQRS.TXT"] != "ABCDEFGHIJKLMNOPQRS.TXT"
                assert "日" in rows[report_key("日本" * 10 + ".dat")]
            if profile == "amigaos-68k-latin1":
                assert rows["N" * 30] == "N" * 30
                assert rows["M" * 31] != "M" * 31
            if profile == "macos-classic-hfs-v1":
                assert rows["M" * 31] == "M" * 31
            if profile in ("amigaos-68k-latin1", "macos-classic-hfs-v1"):
                assert "é" in rows[report_key("é" * 35 + ".txt")]
            if profile in ("dos-cp850-83", "dos-cp437-83"):
                assert rows[report_key("é" * 35 + ".txt")].startswith("éé")
            if modern and profile != "plan9-utf8-v1":
                assert rows[report_key("control\tfile.txt")] == "control\\x09file.txt"
            if profile == "plan9-utf8-v1":
                assert "\\x09" not in rows[report_key("control\tfile.txt")]
            for name, contents in files.items():
                mapped = rows[report_key(name)]
                if modern:
                    mapped = re.sub(r"\\x([0-9A-F]{2})", lambda m: chr(int(m.group(1), 16)), mapped)
                else:
                    mapped = mapped.replace("\\", "/")
                if profile == "macos-classic-hfs-v1":
                    mapped = mapped.replace(":", "/")
                if profile == "riscos-latin1-v1":
                    mapped = mapped.replace(".", "/")
                if profile == "sinclair-ql-latin1-v1":
                    matches = [p for p in (root / "out").rglob("*")
                               if p.is_file() and p.read_bytes() == contents]
                    assert len(matches) == 1
                    assert matches[0].relative_to(root / "out").as_posix().replace("/", "_") == mapped
                else:
                    assert (root / "out" / mapped).read_bytes() == contents
    # A DOS target must not inherit Atari's 1,024-name alias-index ceiling.
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        names = ["F%04d.TXT" % n for n in range(1030)]
        for name in names:
            (root / name).write_bytes(b"x")
        run(exe, root, "create", "--target-profile=dos-cp850-83",
            "--target-auto-rename", "archive.blt", *names)
        run(reader, root, "extract", "-o", "out", "archive.blt")
        assert len((root / "out" / "BPNAMES.TXT").read_text("cp850").splitlines()) == 1032
    print("Target auto-rename: PASS")


if __name__ == "__main__":
    main()
