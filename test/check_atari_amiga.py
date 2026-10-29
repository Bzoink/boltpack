#!/usr/bin/env python3
"""Run Atari FAT12 archive creation followed by Amiga guest extraction."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


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
    print("PASS: Atari FAT12 -> Amiga FS-UAE transfer; evidence:", evidence)


if __name__ == "__main__":
    main()
