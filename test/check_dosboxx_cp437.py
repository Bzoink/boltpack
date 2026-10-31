#!/usr/bin/env python3
"""Verify DOS16 CP437 filename extraction on a DOSBox-X FAT12 floppy."""
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
import tempfile
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
EXPECTED_PATH = ROOT / "test/reference/interoperability/dos16-fat12.json"
EXPECTED = json.loads(EXPECTED_PATH.read_text(encoding="utf-8"))


def check_manifest_file(path, key):
    expected = EXPECTED["files"][key]
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != expected["bytes"] or digest != expected["sha256"]:
        raise RuntimeError("independent expected-result mismatch for %s: %d bytes, sha256=%s" %
                           (key, len(data), digest))


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact_index(root):
    return {
        path.relative_to(root).as_posix(): {
            "size": path.stat().st_size,
            "sha256": sha256(path),
        }
        for path in sorted(root.rglob("*"))
        if path.is_file() and path != root / "run.json"
    }


def command_version(command):
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=10, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable: " + str(exc)
    first = result.stdout.strip().splitlines()[0] if result.stdout.strip() else "unknown"
    return first if result.returncode == 0 else "%s (exit %d)" % (first, result.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox-x", default="dosbox-x")
    parser.add_argument("--output", type=Path,
                        help="new directory for retained floppy image and logs")
    args = parser.parse_args()
    output_root = ROOT / "build"
    output_root.mkdir(exist_ok=True)
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(
        prefix="r02-dosboxx-cp437-", dir=output_root))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    image = out / "dos16-fat12.img"
    source = out / "source"
    source.mkdir()
    (source / "café.txt").write_bytes(b"CP437 filename on FAT12\n")
    os.utime(source / "café.txt", (951786000, 951786000))
    (source / "SRC.TXT").write_bytes(b"DOS16 FAT12 to Linux\n")
    (source / "NEW.TXT").write_bytes(b"DOS16 FAT12 replacement\n")
    check_manifest_file(source / "café.txt", "cafe")
    check_manifest_file(source / "SRC.TXT", "src")
    check_manifest_file(source / "NEW.TXT", "new")
    shutil.copyfile(ROOT / "build/dos16/bpmini.exe", out / "BPMINI.EXE")
    linux = ROOT / "build/linux/boltpack"
    run([str(linux), "c", "--store", "OEM.BLT", "café.txt"], cwd=source)
    shutil.copyfile(source / "OEM.BLT", out / "OEM.BLT")
    shutil.copyfile(source / "SRC.TXT", out / "SRC.TXT")
    shutil.copyfile(source / "NEW.TXT", out / "NEW.TXT")
    run(["mformat", "-i", str(image), "-C", "-f", "720", "::"])
    for name in ("BPMINI.EXE", "OEM.BLT", "SRC.TXT", "NEW.TXT"):
        run(["mcopy", "-i", str(image), str(out / name), "::/" + name])
    run(["mmd", "-i", str(image), "::/OUT"])

    commands = [
        "set BP_TZ_OFFSET=0",
        "bpmini c DOS.BLT SRC.TXT > CREATE.LOG",
        "if errorlevel 1 echo FAIL > CREATE.BAD",
        "bpmini c REPL.BLT SRC.TXT > REPL1.LOG",
        "if errorlevel 1 echo FAIL > REPL1.BAD",
        "attrib +h +s +a REPL.BLT",
        "bpmini c REPL.BLT NEW.TXT > REPL2.LOG",
        "if errorlevel 1 echo FAIL > REPL2.BAD",
        "bpmini x OEM.BLT OUT > X.LOG",
        "if errorlevel 1 echo PARTIAL > X.BAD",
        "exit",
    ]
    (out / "CHECK.BAT").write_text("\r\n".join(commands) + "\r\n")
    run(["mcopy", "-i", str(image), str(out / "CHECK.BAT"), "::/CHECK.BAT"])
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    emulator = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch",
                "-c", "imgmount a " + str(image) + " -t floppy", "-c", "a:",
                "-c", "CHECK.BAT", "-c", "exit"]
    output = run(emulator, env=env, timeout=60, cwd=out)
    (out / "dosboxx.log").write_bytes(output)
    version = subprocess.run([args.dosbox_x, "--version"], check=False,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "dosboxx-version.txt").write_bytes(version.stdout)
    version = subprocess.run([args.dosbox_x, "--version"], check=False,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT).stdout.decode("utf-8", "replace")
    (out / "dosboxx-version.txt").write_text(version)

    extracted = out / "cafe.txt"
    run(["mcopy", "-i", str(image), "::/OUT/CAFé.TXT", str(extracted)])
    check_manifest_file(extracted, "cafe")
    dos_archive = out / "dos-created.blt"
    run(["mcopy", "-i", str(image), "::/DOS.BLT", str(dos_archive)])
    dos_listing = run([str(linux), "l", str(dos_archive)]).decode("utf-8", "replace")
    (out / "linux-listing.txt").write_text(dos_listing)
    extracted_dos = out / "linux-extract"
    extracted_dos.mkdir()
    linux_extract = subprocess.run([str(linux), "x", "-o", str(extracted_dos), str(dos_archive)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(linux_extract.stdout)
    if (linux_extract.returncode != 1 or b"not restored" not in linux_extract.stdout or
            "SRC.TXT" not in dos_listing or
            not (extracted_dos / "SRC.TXT").is_file()):
        raise RuntimeError("Linux could not verify DOS16 FAT12-created archive; evidence: " + str(out))
    check_manifest_file(extracted_dos / "SRC.TXT", "src")
    replacement_archive = out / "dos-replaced.blt"
    run(["mcopy", "-i", str(image), "::/REPL.BLT", str(replacement_archive)])
    replacement_listing = run([str(linux), "l", str(replacement_archive)]).decode("utf-8", "replace")
    if "NEW.TXT" not in replacement_listing or "SRC.TXT" in replacement_listing:
        raise RuntimeError("DOS16 did not publish the replacement archive; evidence: " + str(out))
    replacement_output = out / "replacement-linux-extract"
    replacement_output.mkdir()
    replacement_extract = subprocess.run(
        [str(linux), "x", "-o", str(replacement_output), str(replacement_archive)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "replacement-linux-extract.log").write_bytes(replacement_extract.stdout)
    replacement_member = replacement_output / "NEW.TXT"
    if replacement_extract.returncode not in (0, 1) or not replacement_member.is_file() or \
            (replacement_output / "SRC.TXT").exists():
        raise RuntimeError("Linux could not independently extract the DOS16 replacement archive; evidence: " + str(out))
    check_manifest_file(replacement_member, "new")
    replacement_attributes = run(["mattrib", "-i", str(image), "::/REPL.BLT"]).decode("utf-8", "replace")
    if any(flag not in replacement_attributes for flag in ("A", "H", "S")):
        raise RuntimeError("DOS16 did not preserve FAT archive/hidden/system attributes: " + replacement_attributes)
    listing = run(["mdir", "-i", str(image), "::/OUT"]).decode("utf-8", "replace")
    (out / "fat-listing.txt").write_text(listing)
    if EXPECTED["mdir_filename"] not in listing or \
            EXPECTED["fat_timestamp"].split()[0] not in listing or \
            EXPECTED["fat_timestamp"].split()[1] not in listing:
        raise RuntimeError("FAT12 filename or restored timestamp differs; evidence: " + str(out))

    partial = subprocess.run(["mdir", "-i", str(image), "::/X.BAD"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if partial.returncode == 0:
        run(["mcopy", "-i", str(image), "::/X.BAD", str(out / "X.BAD")])
        status = "partial reported after successful FAT12 byte extraction"
    else:
        status = "complete"
    if not dos_archive.exists() or not replacement_archive.exists():
        raise RuntimeError("DOS16 did not create archives on FAT12; evidence: " + str(out))
    result_text = ("PASS: CP437 extraction, DOS16-to-Linux archive creation, archive replacement, "
                   "and FAT attribute preservation verified on DOSBox-X FAT12\nresult=" +
                   status + "\n")
    (out / "result.txt").write_text(result_text, encoding="utf-8")
    try:
        locale_name = locale.setlocale(locale.LC_ALL, None)
    except locale.Error:
        locale_name = "unknown"
    git_status = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=ROOT)
    (out / "git-status.txt").write_bytes(git_status)
    (out / "git-diff.patch").write_bytes(diff)
    mini = ROOT / "build/dos16/bpmini.exe"
    sources = (Path(__file__).resolve(), EXPECTED_PATH, ROOT / "Makefile", linux, mini)
    record = {
        "case": "DOS16 CP437 filename extraction and FAT12 interoperability",
        "result": result_text.strip(),
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                             cwd=ROOT, text=True).strip(),
        "worktree_status": git_status.decode("utf-8", "replace").splitlines(),
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "environment": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "locale": locale_name,
            "timezone": list(time.tzname),
            "dosbox_x": command_version([args.dosbox_x, "--version"]),
            "mformat": command_version(["mformat", "--version"]),
        },
        "filesystem": "720 KiB FAT12 floppy image under DOSBox-X",
        "commands": {
            "linux_create": [str(linux), "c", "--store", "OEM.BLT", "café.txt"],
            "guest_batch": commands,
            "emulator": emulator,
            "linux_list": [str(linux), "l", str(dos_archive)],
            "linux_extract": [str(linux), "x", "-o", str(extracted_dos), str(dos_archive)],
            "linux_extract_replacement": [str(linux), "x", "-o", str(replacement_output),
                                          str(replacement_archive)],
        },
        "expected_manifest": {
            "path": EXPECTED_PATH.relative_to(ROOT).as_posix(),
            "size": EXPECTED_PATH.stat().st_size,
            "sha256": sha256(EXPECTED_PATH),
        },
        "source_sha256": {
            path.relative_to(ROOT).as_posix(): sha256(path)
            for path in sources if path.is_relative_to(ROOT)
        },
        "inputs": {
            "linux_cli_sha256": sha256(linux),
            "dos16_mini_sha256": sha256(mini),
        },
        "artifacts": artifact_index(out),
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("PASS: DOS16 CP437 extraction, creation, replacement, and FAT attribute preservation under DOSBox-X FAT12;", status + "; evidence:", out / "run.json")


if __name__ == "__main__":
    main()
