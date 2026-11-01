#!/usr/bin/env python3
"""Verify that the CLI exposes its supported target profiles in help."""

import argparse
import pathlib
import re
import subprocess


PROFILES = {
    "atari-st", "dos-cp850-83", "dos-cp437-83", "dos-cp850-lfn-v1",
    "dos16-cp850-mini", "win16-cp437-mini", "os2-16-cp850-mini",
    "os2-fat-cp850", "amigaos-68k-latin1", "macos-classic-hfs-v1",
    "sinclair-ql-latin1-v1", "riscos-latin1-v1", "x68000-sjis-v1",
    "cpm86-83", "linux-posix-v1", "linux-mini-v1", "posix-utf8-v1",
    "plan9-utf8-v1", "haiku-bfs-v1",
}


def output(exe, *args):
    result = subprocess.run([exe, *args], capture_output=True, text=True, check=True)
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--boltpack", required=True)
    exe = str(pathlib.Path(parser.parse_args().boltpack).resolve())
    for option in ("--help", "--help-advanced"):
        assert "--help-targets" in output(exe, option)
    help_text = output(exe, "--help-targets")
    assert output(exe, "help", "targets") == help_text
    assert output(exe, "create", "--help-targets") == help_text
    listed = set(re.findall(r"^  ([a-z][a-z0-9-]+) {2,}", help_text, re.MULTILINE))
    assert listed == PROFILES, (sorted(PROFILES - listed), sorted(listed - PROFILES))
    for token in ("--target-profile=", "--target-auto-rename", "BPNAMES.TXT",
                  "BPNAMESTXT", "BoltLite", "32 KiB", "Shift-JIS"):
        assert token in help_text, token
    print("Target help: PASS")


if __name__ == "__main__":
    main()
