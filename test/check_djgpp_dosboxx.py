#!/usr/bin/env python3
"""Exercise the full DJGPP DOS CLI on DOSBox-X FAT12 with CWSDPMI."""
import argparse
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
PAYLOAD = b"DJGPP DOS FAT12 interoperability probe\r\n"
APPENDED = b"DJGPP DOS archive append probe\r\n"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact_index(directory, manifest_path):
    entries = []
    for path in sorted(directory.rglob("*")):
        if path.is_file() and path != manifest_path:
            entries.append({"path": str(path.relative_to(directory)),
                            "size": path.stat().st_size,
                            "sha256": sha256(path)})
    return entries


def command_version(command):
    try:
        output = subprocess.run(command, check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=15).stdout
        return output.decode("utf-8", "replace").strip().splitlines()[:4]
    except (OSError, subprocess.TimeoutExpired) as error:
        return ["unavailable: " + str(error)]


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox-x", default="dosbox-x")
    parser.add_argument("--dos-cc", default="i586-pc-msdosdjgpp-gcc",
                        help="DJGPP cross compiler used to build the DOS executable")
    parser.add_argument("--cwsdpmi", type=Path, required=True,
                        help="path to CWSDPMI.EXE from the DJGPP csdpmi package")
    parser.add_argument("--output", type=Path,
                        help="new directory for retained floppy image and logs")
    args = parser.parse_args()
    out = args.output.resolve() if args.output else Path(
        tempfile.mkdtemp(prefix="boltpack-djgpp-dosboxx-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    dpmi = args.cwsdpmi.resolve()
    if not dpmi.is_file():
        raise SystemExit("CWSDPMI executable not found: " + str(dpmi))
    executable = ROOT / "build/dos-djgpp/boltpack.exe"
    if not executable.is_file():
        raise SystemExit("build the DJGPP executable first: make dos-djgpp")

    image = out / "djgpp-fat12.img"
    source = out / "SRC.TXT"
    source.write_bytes(PAYLOAD)
    appended = out / "ADD.TXT"
    appended.write_bytes(APPENDED)
    shutil.copyfile(executable, out / "BOLTPACK.EXE")
    shutil.copyfile(dpmi, out / "CWSDPMI.EXE")
    rename_probe = out / "RENAMER.EXE"
    run([args.dos_cc, "-O2", "-Wall", "-Wextra",
         str(ROOT / "test/dos_rename_noreplace_probe.c"), "-o", str(rename_probe)])
    linux = ROOT / "build/linux/boltpack"
    run([str(linux), "c", "LINUX.BLT", "SRC.TXT"], cwd=out)
    run(["mformat", "-i", str(image), "-C", "-f", "720", "::"])
    for name in ("BOLTPACK.EXE", "CWSDPMI.EXE", "RENAMER.EXE",
                 "SRC.TXT", "ADD.TXT", "LINUX.BLT"):
        run(["mcopy", "-i", str(image), str(out / name), "::/" + name])
    run(["mmd", "-i", str(image), "::/OUT"])

    commands = [
        "BOLTPACK c DOS.BLT SRC.TXT",
        "attrib +h +s +a DOS.BLT",
        "BOLTPACK a DOS.BLT ADD.TXT",
        "BOLTPACK d DOS.BLT SRC.TXT",
        "BOLTPACK t DOS.BLT",
        "BOLTPACK t LINUX.BLT",
        "BOLTPACK x -o OUT LINUX.BLT",
        "RENAMER.EXE",
        "exit",
    ]
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    emulator = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch",
                "-c", "imgmount a " + str(image) + " -t floppy",
                "-c", "a:"]
    for command in commands:
        emulator.extend(("-c", command))
    output = run(emulator, env=env, timeout=150, cwd=out)
    (out / "dosboxx.log").write_bytes(output)

    dos_extracted = out / "dos-extracted.txt"
    dos_archive = out / "dos-created.blt"
    run(["mcopy", "-i", str(image), "::/OUT/SRC.TXT", str(dos_extracted)])
    run(["mcopy", "-i", str(image), "::/DOS.BLT", str(dos_archive)])
    rename_report = out / "rename-probe-report.txt"
    run(["mcopy", "-i", str(image), "::/RENAMER.TXT", str(rename_report)])
    rename_result = rename_report.read_text(encoding="ascii").strip()
    rename_fields = dict(field.split("=", 1) for field in rename_result.split())
    if (rename_fields.get("rename_rc") != "-1" or
            rename_fields.get("source_preserved") != "1" or
            rename_fields.get("target_preserved") != "1"):
        raise RuntimeError("DOS rename did not fail without replacing an existing target: " +
                           rename_result)
    attributes = run(["mattrib", "-i", str(image), "::/DOS.BLT"]).decode("utf-8", "replace")
    if any(flag not in attributes for flag in ("A", "H", "S")):
        raise RuntimeError("DJGPP DOS edits did not preserve FAT archive flags: " + attributes)
    if dos_extracted.read_bytes() != PAYLOAD:
        raise RuntimeError("DJGPP DOS extraction differed from source bytes")
    run([str(linux), "t", str(dos_archive)])
    listing = run([str(linux), "l", str(dos_archive)]).decode("utf-8", "replace")
    if "ADD.TXT" not in listing or "SRC.TXT" in listing:
        raise RuntimeError("DJGPP DOS append/delete did not publish expected members")
    linux_out = out / "linux-extracted"
    linux_out.mkdir()
    extract = subprocess.run([str(linux), "x", "-o", str(linux_out), str(dos_archive)],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(extract.stdout)
    if extract.returncode not in (0, 1) or \
            not (linux_out / "ADD.TXT").is_file() or \
            (linux_out / "ADD.TXT").read_bytes() != APPENDED or \
            (linux_out / "SRC.TXT").exists():
        raise RuntimeError("Linux could not extract the DJGPP-created archive; evidence: " + str(out))
    (out / "result.txt").write_text(
        "PASS: DJGPP DOS CLI created, appended, deleted, and tested an archive on DOSBox-X FAT12; "
        "DOS extracted a Linux-created archive and Linux extracted the edited DOS archive byte-identically\n",
        encoding="utf-8")
    revision = command_version(["git", "-C", str(ROOT), "rev-parse", "HEAD"])
    status = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain=v1",
                            "--untracked-files=all"], check=False,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=15).stdout.decode("utf-8", "replace").splitlines()
    diff = subprocess.run(["git", "-C", str(ROOT), "diff", "--binary", "HEAD"],
                          check=False, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=30).stdout
    source_paths = ["Makefile", "src/frontend/dos/main.c",
                    "src/frontend/dos/dos_rename.h",
                    "test/check_djgpp_dosboxx.py",
                    "test/dos_rename_noreplace_probe.c"]
    manifest_path = out / "manifest.json"
    manifest = {
        "result": "PASS",
        "test": "full DJGPP DOS CLI bidirectional FAT12 interoperability",
        "source": {
            "git_revision": revision[0] if revision else "unavailable",
            "worktree_dirty": bool(status),
            "worktree_status_entries": len(status),
            "worktree_status_sha256": hashlib.sha256(
                ("\n".join(status) + "\n").encode("utf-8")).hexdigest(),
            "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
            "source_file_sha256": {name: sha256(ROOT / name)
                                   for name in source_paths},
        },
        "host": {"platform": platform.platform(), "python": platform.python_version(),
                 "locale": locale.setlocale(locale.LC_ALL, None),
                 "timezone": {"tz_env": os.environ.get("TZ"),
                             "tzname": list(time.tzname),
                             "seconds_west": time.timezone}},
        "tools": {
            "dosbox_x": command_version([args.dosbox_x, "--version"]),
            "djgpp_compiler": command_version([args.dos_cc, "--version"]),
            "cwsdpmi_sha256": sha256(dpmi),
            "mtools_mformat": command_version(["mformat", "--version"]),
        },
        "build": {
            "target": "dos-djgpp",
            "make_variables": {"DOS_CC": args.dos_cc},
            "compiler_flags": ["-ansi", "-pedantic", "-Wall", "-Wextra",
                               "-Wno-unused-function", "-O2", "-ffunction-sections",
                               "-fdata-sections", "-U__STRICT_ANSI__"],
            "network": "disabled",
            "zstd": "disabled",
            "dos_executable_sha256": sha256(executable),
            "linux_executable_sha256": sha256(linux),
        },
        "filesystem": {"type": "FAT12", "image_bytes": image.stat().st_size,
                       "image_sha256": sha256(image)},
        "inputs": {"source_sha256": hashlib.sha256(PAYLOAD).hexdigest(),
                   "appended_sha256": hashlib.sha256(APPENDED).hexdigest()},
        "observed": {
            "dos_commands": commands,
            "dos_operations": ["create", "set HIDDEN/SYSTEM/ARCHIVE", "append",
                               "delete", "test", "extract Linux-created archive",
                               "refuse rename over existing target on FAT12"],
            "linux_operations": ["test edited DOS archive", "extract edited DOS archive"],
            "payload_bytes_match": True,
            "archive_fat_flags_preserved": ["HIDDEN", "SYSTEM", "ARCHIVE"],
            "rename_existing_target_refused": True,
            "rename_probe_result": rename_result,
        },
        "race_probe": {
            "source_sha256": sha256(ROOT / "test/dos_rename_noreplace_probe.c"),
            "executable_sha256": sha256(rename_probe),
            "destination_sentinel_preserved": True,
            "source_object_preserved": True,
        },
    }
    manifest["artifacts"] = artifact_index(out, manifest_path)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                             encoding="utf-8")
    print("PASS: DJGPP DOS CLI create/append/delete/test/extract on DOSBox-X FAT12; evidence:", out)


if __name__ == "__main__":
    main()
