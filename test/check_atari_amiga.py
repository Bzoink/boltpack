#!/usr/bin/env python3
"""Run Atari FAT12 archive creation followed by Amiga guest extraction."""
import argparse
import datetime
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
EXPECTED_MANIFESTS = (
    ROOT / "test/reference/interoperability/atari-amiga-fsuae.json",
    ROOT / "test/reference/interoperability/amiga-host-directory.json",
)


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
    records = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and path != root / "run.json":
            records[path.relative_to(root).as_posix()] = {
                "size": path.stat().st_size,
                "sha256": sha256(path),
            }
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--workbench", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--fs-uae", default="fs-uae")
    parser.add_argument("--xdftool", required=True)
    parser.add_argument("--atari-cli", type=Path,
                        default=ROOT / "build/atari/boltpack.tos")
    parser.add_argument("--amiga-cli", type=Path,
                        default=ROOT / "build/amiga/boltpack")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.output:
        evidence = args.output.resolve()
        evidence.mkdir(parents=True, exist_ok=False)
    else:
        evidence = Path(tempfile.mkdtemp(prefix="boltpack-atari-amiga-"))
    producer = evidence / "atari-producer"
    consumer = evidence / "amiga-consumer"
    commands = [
        ["python3", str(ROOT / "test/check_hatari_fat.py"),
         "--tos", str(args.tos), "--hatari", args.hatari,
         "--hconsole", str(args.hconsole), "--atari-cli", str(args.atari_cli),
         "--linux-cli", str(args.linux_cli), "--output", str(producer)],
        ["python3", str(ROOT / "test/check_fsuae_interop.py"),
         "--rom", str(args.rom), "--workbench", str(args.workbench),
         "--fs-uae", args.fs_uae, "--xdftool", args.xdftool,
         "--amiga-cli", str(args.amiga_cli), "--linux-cli", str(args.linux_cli),
         "--atari-archive", str(producer / "atari.blt"),
         "--export-amiga-archive", "--output", str(consumer)],
        ["python3", str(ROOT / "test/check_hatari_fat.py"),
         "--tos", str(args.tos), "--hatari", args.hatari,
         "--hconsole", str(args.hconsole), "--atari-cli", str(args.atari_cli),
         "--linux-cli", str(args.linux_cli), "--amiga-archive",
         str(consumer / "amiga-created.blt"), "--output",
         str(evidence / "atari-consumer")],
    ]
    for index, command in enumerate(commands):
        log_path = evidence / ("producer.log" if index == 0 else
                               "amiga-consumer.log" if index == 1 else
                               "atari-consumer.log")
        with log_path.open("wb") as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log,
                                   stderr=subprocess.STDOUT)
        if result.returncode != 0:
            raise SystemExit("cross-system fixture failed; inspect %s and %s" %
                             (evidence, log_path))
    status = subprocess.check_output(["git", "status", "--porcelain"],
                                     cwd=ROOT)
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"],
                                   cwd=ROOT)
    (evidence / "git-status.txt").write_bytes(status)
    (evidence / "git-diff.patch").write_bytes(diff)
    try:
        locale_name = locale.setlocale(locale.LC_ALL, None)
    except locale.Error:
        locale_name = "unknown"
    expected_manifests = {
        path.relative_to(ROOT).as_posix(): {
            "sha256": sha256(path),
            "size": path.stat().st_size,
        }
        for path in EXPECTED_MANIFESTS
    }
    source_paths = (
        ROOT / "Makefile",
        ROOT / "test/check_atari_amiga.py",
        ROOT / "test/check_hatari_fat.py",
        ROOT / "test/check_fsuae_interop.py",
        ROOT / "test/reference/interoperability/atari-amiga-fsuae.json",
        ROOT / "test/reference/interoperability/amiga-host-directory.json",
        ROOT / "src/frontend/atari/main.c",
        ROOT / "src/frontend/amiga/main.c",
        ROOT / "src/core/archiver.c",
        ROOT / "src/core/compress.c",
    )
    result = {
        "purpose": "Atari FAT12 -> Amiga FS-UAE -> Atari FAT12 archive transfer",
        "result": "PASS: both directions verified against independent payload expectations",
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
            "fs_uae": command_version([args.fs_uae, "--version"]),
            "atari_compiler": command_version(["m68k-atari-mint-gcc", "--version"]),
            "amiga_compiler": command_version(
                ["/opt/amiga/bin/m68k-amigaos-gcc", "--version"]),
            "xdftool": str(Path(args.xdftool).resolve()),
            "xdftool_amitools": command_version([
                str(Path(args.xdftool).resolve().parent / "python"), "-c",
                "from importlib.metadata import version; print(version('amitools'))",
            ]),
        },
        "filesystems": {
            "atari": "FAT12 floppy under Hatari/TOS",
            "amiga": "FS-UAE Workbench host-directory filesystem",
        },
        "commands": commands,
        "expected_manifests": expected_manifests,
        "inputs": {
            "tos_sha256": sha256(args.tos.expanduser().resolve(strict=True)),
            "kickstart_sha256": sha256(args.rom.expanduser().resolve(strict=True)),
            "workbench_adf_sha256": sha256(
                args.workbench.expanduser().resolve(strict=True)),
            "hatari_hconsole_sha256": sha256(
                args.hconsole.expanduser().resolve(strict=True)),
            "atari_cli_sha256": sha256(args.atari_cli.resolve(strict=True)),
            "amiga_cli_sha256": sha256(args.amiga_cli.resolve(strict=True)),
            "linux_cli_sha256": sha256(args.linux_cli.resolve(strict=True)),
        },
        "source_sha256": {
            path.relative_to(ROOT).as_posix(): sha256(path)
            for path in source_paths if path.is_file()
        },
        "child_runs": {
            "atari_producer": "atari-producer/manifest.txt",
            "amiga_consumer": "amiga-consumer/manifest.txt",
            "atari_consumer": "atari-consumer/manifest.txt",
        },
        "artifacts": artifact_index(evidence),
        "limitations": [
            "Atari filesystem evidence is TOS 1.62 FAT12 under Hatari.",
            "Amiga transfer uses the FS-UAE host-directory backend; it is not OFS/FFS qualification.",
            "The three child manifests contain the detailed per-platform outcomes.",
        ],
    }
    (evidence / "run.json").write_text(json.dumps(result, indent=2) + "\n",
                                       encoding="utf-8")
    print("PASS: Atari FAT12 -> Amiga FS-UAE transfer; evidence:", evidence)


if __name__ == "__main__":
    main()
