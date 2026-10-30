#!/usr/bin/env python3
"""Exercise Linux/Atari CLI exchange on Hatari's GEMDOS host-directory drive."""
import argparse
import datetime
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = json.loads((ROOT / "test/reference/interoperability/atari-host-directory.json").read_text())
EXPECTED_PATH = ROOT / "test/reference/interoperability/atari-host-directory.json"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_version(command):
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=10, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable: " + str(exc)
    return result.stdout.strip().splitlines()[0] if result.stdout.strip() else \
        "unknown (exit %d)" % result.returncode


def artifact_index(root):
    return {
        path.relative_to(root).as_posix(): {
            "size": path.stat().st_size,
            "sha256": sha256(path),
        }
        for path in sorted(root.rglob("*"))
        if path.is_file() and path != root / "run.json"
    }


def check_manifest_file(path, relative_name):
    expected = EXPECTED["files"][relative_name]
    if expected.get("type") == "directory":
        if not path.is_dir():
            raise RuntimeError("independent expected-result mismatch: missing directory %s" %
                               relative_name)
        return
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != expected["bytes"] or digest != expected["sha256"]:
        raise RuntimeError("independent expected-result mismatch for %s: %d bytes, sha256=%s" %
                           (relative_name, len(data), digest))


def run(args, cwd, log, env=None, timeout=60, expected=(0,)):
    result = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    with log.open("ab") as f:
        f.write(("$ " + repr(args) + "\n").encode())
        f.write(result.stdout + b"\n")
        f.write(("exit: %d\n" % result.returncode).encode())
    if result.returncode not in expected:
        raise RuntimeError("command failed (%d); see %s" % (result.returncode, log))
    return result.stdout.decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", required=True, type=Path)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--cc", default="m68k-atari-mint-gcc")
    parser.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    parser.add_argument("--atari-cli", type=Path, default=ROOT / "build/atari/boltpack.tos")
    parser.add_argument("--output", type=Path, help="new directory in which to retain evidence")
    args = parser.parse_args()
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="atari-host-directory-", dir=build))
    drive = output / "drive"
    drive.mkdir()
    log = output / "run.log"
    tos = args.tos.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    run([args.hatari, "--version"], ROOT, log, expected=(0, 1))
    shutil.copyfile(args.atari_cli.resolve(strict=True), drive / "BOLTPACK.TOS")
    run([args.cc, "-O2", str(ROOT / "test/atari/guest_runner.c"), "-o",
         str(drive / "RUN.TOS")], ROOT, log)
    data = (b"BoltPack Atari exchange 0123456789\n" * 400) + bytes(range(256))
    (drive / "SRC.TXT").write_bytes(data)
    added = b"file appended on Atari\n" * 100
    (drive / "NEW.TXT").write_bytes(added)
    tree = drive / "TREE"
    (tree / "NEST" / "EMPTY").mkdir(parents=True)
    (tree / "NEST" / "FILE.TXT").write_bytes(added)
    check_manifest_file(drive / "SRC.TXT", "SRC.TXT")
    check_manifest_file(drive / "NEW.TXT", "NEW.TXT")
    check_manifest_file(tree / "NEST" / "FILE.TXT", "TREE/NEST/FILE.TXT")
    check_manifest_file(tree / "NEST" / "EMPTY", "TREE/NEST/EMPTY")
    run([str(linux), "c", "--interchange-profile-1", "TREEIN.BLT", "TREE"], drive, log)
    name_fixture = output / "names"
    name_fixture.mkdir()
    (name_fixture / "TOOLONGNAME.TXT").write_bytes(b"long name\n")
    run([str(linux), "c", "--interchange-profile-1", str(drive / "LONG.BLT"),
         "TOOLONGNAME.TXT"], name_fixture, log)
    (name_fixture / "FOO.TXT").write_bytes(b"upper\n")
    (name_fixture / "foo.txt").write_bytes(b"lower\n")
    run([str(linux), "c", "--interchange-profile-1", str(drive / "COLLIDE.BLT"),
         "FOO.TXT", "foo.txt"], name_fixture, log)
    (name_fixture / "Foo").mkdir()
    (name_fixture / "foo").mkdir()
    (name_fixture / "Foo" / "A.TXT").write_bytes(b"first\n")
    (name_fixture / "foo" / "B.TXT").write_bytes(b"second\n")
    run([str(linux), "c", "--interchange-profile-1", str(drive / "PARENT.BLT"),
         "Foo/A.TXT", "foo/B.TXT"], name_fixture, log)
    for suffix, method in (("0", "-0"), ("1", "-1")):
        (drive / ("OUT" + suffix)).mkdir()
        run([str(linux), "c", "--interchange-profile-1", method,
             "LINUX%s.BLT" % suffix, "SRC.TXT"], drive, log)
    original = (drive / "LINUX0.BLT").read_bytes()
    shutil.copyfile(drive / "LINUX1.BLT", drive / "EDIT.BLT")
    corrupt = bytearray(original)
    payload_at = corrupt.find(data)
    if payload_at < 0 or corrupt.find(data, payload_at + 1) >= 0:
        raise RuntimeError("cannot locate unique stored payload to corrupt")
    corrupt[payload_at + len(data) // 2] ^= 1
    (drive / "BAD.BLT").write_bytes(corrupt)
    for directory in ("ADDOUT", "DELOUT", "BADOUT", "TREEOUT", "LONGOUT", "COLOUT", "PAROUT"):
        (drive / directory).mkdir()
    sentinel = b"existing destination must survive checksum failure\n"
    (drive / "BADOUT" / "SRC.TXT").write_bytes(sentinel)
    commands = [
        r"x -o C:\OUT0 C:\LINUX0.BLT",
        r"x -o C:\OUT1 C:\LINUX1.BLT",
        r"c --interchange-profile-1 -0 C:\STORED.BLT C:\SRC.TXT",
        r"c --interchange-profile-1 -1 C:\LITE.BLT C:\SRC.TXT",
        r"t C:\STORED.BLT",
        r"t C:\LITE.BLT",
        r"a --interchange-profile-1 C:\EDIT.BLT C:\NEW.TXT",
        r"x -o C:\ADDOUT C:\EDIT.BLT",
        r"d C:\EDIT.BLT SRC.TXT",
        r"x -o C:\DELOUT C:\EDIT.BLT",
        r"a C:\LINUX0.BLT C:\MISSING.TXT",
        r"d C:\LINUX0.BLT MISSING.TXT",
        r"x -f -o C:\BADOUT C:\BAD.BLT",
        r"x -o C:\TREEOUT C:\TREEIN.BLT",
        r"c --interchange-profile-1 C:\TREEAT.BLT C:\TREE",
        r"x -o C:\LONGOUT C:\LONG.BLT",
        r"x -o C:\COLOUT C:\COLLIDE.BLT",
        r"x -o C:\PAROUT C:\PARENT.BLT",
    ]
    (drive / "COMMANDS.TXT").write_text("\n".join(commands) + "\n", encoding="ascii")
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    run([args.hatari, "--machine", "ste", "--tos", str(tos), "--sound", "off",
         "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
         "--memsize", "4", "--harddrive", str(drive), "--auto", r"C:\RUN.TOS",
         "--run-vbls", "16000"], ROOT, log, env=env)
    results = (drive / "RESULT.TXT").read_text().splitlines()
    expected_results = ["0"] * 10 + ["1"] * 4 + ["0"] + ["1"] * 3
    if results != expected_results:
        raise RuntimeError("guest results %r; expected %r" % (results, expected_results))
    for index, expected in ((10, "cannot stat"), (11, "no matching entries"),
                            (12, "checksum mismatch"), (13, "cannot restore directory timestamp"),
                            (15, "outside the Atari 8.3 profile"), (16, "destination name collision"),
                            (17, "destination name collision")):
        if expected not in (drive / ("LOG%02d.TXT" % index)).read_text(errors="replace"):
            raise RuntimeError("guest failure did not reach expected check: " + expected)
    for suffix in ("0", "1"):
        check_manifest_file(drive / ("OUT" + suffix) / "SRC.TXT", "SRC.TXT")
    check_manifest_file(drive / "ADDOUT" / "SRC.TXT", "SRC.TXT")
    check_manifest_file(drive / "ADDOUT" / "NEW.TXT", "NEW.TXT")
    check_manifest_file(drive / "DELOUT" / "NEW.TXT", "NEW.TXT")
    if (drive / "DELOUT" / "SRC.TXT").exists():
        raise RuntimeError("guest append/delete produced incorrect archive contents")
    if (drive / "LINUX0.BLT").read_bytes() != original:
        raise RuntimeError("failed append or delete modified the original archive")
    if (drive / "BADOUT" / "SRC.TXT").read_bytes() != sentinel:
        raise RuntimeError("checksum failure modified the existing destination")
    if sorted(p.name for p in (drive / "BADOUT").iterdir()) != ["SRC.TXT"]:
        raise RuntimeError("checksum failure left a stage in the extraction directory")
    guest_tree = drive / "TREEOUT" / "TREE" / "NEST"
    check_manifest_file(guest_tree / "FILE.TXT", "TREE/NEST/FILE.TXT")
    if not (guest_tree / "EMPTY").is_dir():
        raise RuntimeError("guest tree extraction lost an empty directory or file")
    for directory in ("LONGOUT", "COLOUT", "PAROUT"):
        if list((drive / directory).iterdir()):
            raise RuntimeError("name preflight changed output directory: " + directory)
    tree_target = output / "linux-tree"
    tree_target.mkdir()
    message = run([str(linux), "x", "-o", str(tree_target), str(drive / "TREEAT.BLT")],
                  ROOT, log, expected=(1,))
    if "some entries or attributes were not restored" not in message:
        raise RuntimeError("expected Atari metadata preservation report for tree")
    linux_tree = tree_target / "TREE" / "nest"
    check_manifest_file(linux_tree / "file.txt", "TREE/NEST/FILE.TXT")
    if not (linux_tree / "empty").is_dir():
        raise RuntimeError("Linux tree extraction lost an empty directory or file")
    for archive in ("STORED.BLT", "LITE.BLT"):
        target = output / ("linux-" + archive)
        target.mkdir()
        message = run([str(linux), "x", "-o", str(target), str(drive / archive)],
                      ROOT, log, expected=(1,))
        if "some entries or attributes were not restored" not in message:
            raise RuntimeError("expected partial metadata preservation: " + archive)
        check_manifest_file(target / "SRC.TXT", "SRC.TXT")
    for archive in ("LINUX1.BLT", "LITE.BLT"):
        info = run([str(linux), "i", str(drive / archive)], ROOT, log)
        if "BoltLite" not in info or (drive / archive).stat().st_size >= len(data) // 2:
            raise RuntimeError("archive did not exercise BoltLite: " + archive)
    for archive in ("LINUX0.BLT", "STORED.BLT"):
        info = run([str(linux), "i", str(drive / archive)], ROOT, log)
        if "BoltLite" in info or (drive / archive).stat().st_size < len(data):
            raise RuntimeError("archive did not use Stored: " + archive)
    (output / "manifest.txt").write_text("backend=Hatari GEMDOS host directory\nrom=" + str(tos) +
        "\nrom_sha256=" + hashlib.sha256(tos.read_bytes()).hexdigest() +
        "\npayload_sha256=" + hashlib.sha256(data).hexdigest() + "\n", encoding="utf-8")
    status = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=ROOT)
    (output / "git-status.txt").write_bytes(status)
    (output / "git-diff.patch").write_bytes(diff)
    try:
        locale_name = locale.setlocale(locale.LC_ALL, None)
    except locale.Error:
        locale_name = "unknown"
    source_paths = (
        ROOT / "Makefile", Path(__file__).resolve(), EXPECTED_PATH,
        ROOT / "src/frontend/atari/main.c", ROOT / "src/core/archiver.c",
        ROOT / "src/core/compress.c", ROOT / "src/core/bp_codepage.c",
    )
    record = {
        "purpose": "Linux/Atari CLI exchange on Hatari GEMDOS host-directory storage",
        "result": "PASS: exchange, edits, expected refusal/partial results, and failure cleanup",
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "worktree_status": status.decode("utf-8", "replace").splitlines(),
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "environment": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "locale": locale_name,
            "timezone": list(time.tzname),
            "hatari": command_version([args.hatari, "--version"]),
            "atari_compiler": command_version([args.cc, "--version"]),
        },
        "filesystem": "GEMDOS host-directory drive under Hatari; not FAT12",
        "commands": "run.log",
        "expected_manifest": {
            "path": EXPECTED_PATH.relative_to(ROOT).as_posix(),
            "size": EXPECTED_PATH.stat().st_size,
            "sha256": sha256(EXPECTED_PATH),
        },
        "inputs": {
            "tos_sha256": sha256(tos),
            "linux_cli_sha256": sha256(linux),
            "atari_cli_sha256": sha256(args.atari_cli.resolve(strict=True)),
            "guest_runner_source_sha256": sha256(ROOT / "test/atari/guest_runner.c"),
        },
        "source_sha256": {
            path.relative_to(ROOT).as_posix(): sha256(path)
            for path in source_paths if path.is_file()
        },
        "artifacts": artifact_index(output),
        "limitations": [
            "This run uses Hatari's GEMDOS host-directory backend, not a native FAT filesystem.",
            "GEMDOS directory timestamp restoration is an expected partial result.",
        ],
    }
    (output / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                     encoding="utf-8")
    print("PASS: Atari exchange, archive edits and failure cleanup; evidence:",
          output / "run.json")


if __name__ == "__main__":
    main()
