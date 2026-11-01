#!/usr/bin/env python3
"""Cross-read Windows or macOS target auto-renamed archives on Linux."""

import argparse
import pathlib
import subprocess
import tempfile


def escaped(name):
    return "".join(chr(c) if 32 <= c <= 126 and c != 92 else
                   "\\\\" if c == 92 else "\\x%02X" % c
                   for c in name.encode("utf-8"))


def run(argv, cwd, partial=False):
    result = subprocess.run(argv, cwd=cwd, capture_output=True, timeout=90)
    if result.returncode and not (partial and result.returncode == 1 and
                                  b"some entries or attributes were not restored" in result.stderr):
        raise AssertionError(f"{argv}: {result.stderr.decode(errors='replace')}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--linux-cli", required=True)
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--platform", choices=("windows", "macos"), default="windows")
    args = parser.parse_args()
    binary = str(pathlib.Path(args.binary).resolve())
    linux = str(pathlib.Path(args.linux_cli).resolve())
    profiles = (("atari-st", "ascii"), ("dos-cp850-83", "cp850"),
                ("dos-cp437-83", "cp437"), ("dos16-cp850-mini", "cp850"),
                ("win16-cp437-mini", "cp437"), ("os2-16-cp850-mini", "cp850"),
                ("os2-fat-cp850", "cp850"), ("dos-cp850-lfn-v1", "cp850"),
                ("amigaos-68k-latin1", "latin1"), ("macos-classic-hfs-v1", "mac_roman"),
                ("sinclair-ql-latin1-v1", "latin1"), ("riscos-latin1-v1", "latin1"),
                ("x68000-sjis-v1", "shift_jis"), ("cpm86-83", "ascii"))
    profiles += (("linux-posix-v1", "utf-8"), ("linux-mini-v1", "utf-8"),
                 ("posix-utf8-v1", "utf-8"), ("plan9-utf8-v1", "utf-8"),
                 ("haiku-bfs-v1", "utf-8"))
    command = [args.wine, binary] if args.platform == "windows" else [binary]
    with tempfile.TemporaryDirectory(prefix="boltpack-native-alias-") as tmp:
        root = pathlib.Path(tmp)
        for help_args in (("--help-targets",), ("help", "targets"),
                          ("create", "--help-targets")):
            help_result = run([*command, *help_args], root)
            help_text = help_result.stdout + help_result.stderr
            for profile, _ in profiles:
                assert profile.encode() in help_text, (args.platform, help_args, profile)
        files = {"Long filename 01.txt": b"one", "Long filename 02.txt": b"two",
                 "café.txt": b"accent", "日本語.txt": b"japanese",
                 "BPNAMES.TXT": b"report collision", "BPNAMESTXT": b"riscos collision",
                 "nested folder/child long filename.txt": b"nested"}
        for name, payload in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(payload)
        for profile, codepage in profiles:
            archive = profile + ".blt"
            out = "out-" + profile
            inputs = [name for name in files if "/" not in name] + ["nested folder"]
            run([*command, "create", "--target-profile=" + profile,
                 "--target-auto-rename", archive, *inputs], root)
            run([linux, "test", archive], root)
            run([linux, "extract", "-o", out, archive], root, partial=True)
            report_name = "BPNAMESTXT" if profile == "riscos-latin1-v1" else "BPNAMES.TXT"
            report = (root / out / report_name).read_bytes().decode(codepage)
            rows = dict(line.split("\t", 1) for line in report.splitlines()[2:])
            for name, payload in files.items():
                mapped = rows[escaped(name)].replace("\\", "/")
                if profile == "macos-classic-hfs-v1":
                    mapped = mapped.replace(":", "/")
                elif profile == "riscos-latin1-v1":
                    mapped = mapped.replace(".", "/")
                if profile == "sinclair-ql-latin1-v1":
                    matches = [p for p in (root / out).rglob("*")
                               if p.is_file() and p.read_bytes() == payload]
                    assert len(matches) == 1
                    assert matches[0].relative_to(root / out).as_posix().replace("/", "_") == mapped
                else:
                    assert (root / out / mapped).read_bytes() == payload, (profile, name, mapped)
        (root / "MiXeD.TxT").write_bytes(b"valid")
        run([*command, "create", "--target-profile=atari-st",
             "--target-name-report", "report-only.blt", "MiXeD.TxT"], root)
        run([linux, "extract", "-o", "report-only", "report-only.blt"], root,
            partial=True)
        assert b"MiXeD.TxT\tMIXED.TXT" in (root / "report-only/BPNAMES.TXT").read_bytes()
        rejected = subprocess.run(
            [*command, "create", "--target-profile=atari-st",
             "--target-name-report", "rejected.blt", "BPNAMES.TXT"],
            cwd=root, capture_output=True, timeout=90)
        assert rejected.returncode != 0 and not (root / "rejected.blt").exists()
        run([*command, "create", "--target-profile=cpm86-83", "-b", "32k",
             "--target-auto-rename", "cpm-32k.blt", "MiXeD.TxT"], root)
        run([linux, "test", "cpm-32k.blt"], root)
        if args.platform == "macos":
            (root / "ソ.txt").write_bytes(b"shift-jis trail")
            (root / "added.dat").write_bytes(b"append")
            run([*command, "create", "--target-profile=x68000-sjis-v1",
                 "x68-strict.blt", "ソ.txt"], root)
            run([*command, "add", "--target-profile=x68000-sjis-v1",
                 "x68-strict.blt", "added.dat"], root)
            run([linux, "test", "x68-strict.blt"], root)
            before = (root / "x68-strict.blt").read_bytes()
            (root / "bad-🧪.txt").write_bytes(b"not shift-jis")
            rejected = subprocess.run(
                [*command, "add", "--target-profile=x68000-sjis-v1",
                 "x68-strict.blt", "bad-🧪.txt"], cwd=root, capture_output=True, timeout=90)
            assert rejected.returncode != 0 and (root / "x68-strict.blt").read_bytes() == before
    print(f"{args.platform} target auto-rename: PASS")


if __name__ == "__main__":
    main()
