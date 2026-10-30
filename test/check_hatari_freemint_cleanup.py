#!/usr/bin/env python3
"""Run Atari GUI cleanup helpers from the production source on FreeMiNT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = (
    "result-file-open", "create-output-root", "create-stage-parent",
    "create-sentinel", "create-stage", "write-recovery-journal",
    "pin-stage-parent", "pin-output-root", "rename-open-root-refused",
    "held-parent-stage-cleanup", "held-root-journal-cleanup",
    "no-stale-stage-warning", "sentinel-preserved",
)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command, cwd, timeout=180):
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True,
                          timeout=timeout, check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emutos", required=True, type=Path,
                        help="EmuTOS ROM image")
    parser.add_argument("--freemint-root", required=True, type=Path,
                        help="FreeMiNT boot tree containing AUTO/ and MINT/")
    parser.add_argument("--probe-prg", required=True, type=Path,
                        help="test build compiled from atari_gui/main.c")
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--output-root", type=Path,
                        default=ROOT / "build/freemint-probe")
    args = parser.parse_args()

    rom = args.emutos.expanduser().resolve(strict=True)
    freemint_root = args.freemint_root.expanduser().resolve(strict=True)
    probe = args.probe_prg.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="production-cleanup-", dir=output_root))
    diskroot = out / "diskroot"
    shutil.copytree(freemint_root, diskroot)
    if not (diskroot / "AUTO").is_dir() or not (diskroot / "MINT").is_dir():
        parser.error("--freemint-root must contain AUTO/ and MINT/")
    configs = list(diskroot.glob("MINT/*/MINT.CNF"))
    if not configs:
        parser.error("FreeMiNT boot tree has no MINT/<version>/MINT.CNF")
    (diskroot / "PROBE.PRG").write_bytes(probe.read_bytes())
    configs[0].write_text("INIT=C:\\PROBE.PRG\n", encoding="ascii")

    image = out / "cleanup.img"
    image_create = run(["atari-hd-image", "8", str(image), "PROBE",
                        str(diskroot)], out)
    (out / "image-create.log").write_text(image_create.stdout, encoding="utf-8")
    if image_create.returncode != 0:
        raise RuntimeError("atari-hd-image failed; evidence: " + str(out))

    commands_file = out / "hconsole.txt"
    commands_file.write_text("sleep 30\nscreenshot\nsleep 1\nquit\n",
                             encoding="ascii")
    env = os.environ.copy()
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    command = [
        sys.executable, str(hconsole), str(commands_file), "--exit", "--",
        "--machine", "ste", "--memsize", "4", "--tos", str(rom),
        "--acsi", "0=" + str(image), "--configfile",
        str(ROOT / "build/freemint-probe/hatari.cfg"),
        "--confirm-quit", "false", "--fast-forward", "true", "--sound", "off",
    ]
    hatari_run = run(command, out, timeout=150)
    (out / "hatari.log").write_text(hatari_run.stdout, encoding="utf-8")
    if hatari_run.returncode != 0:
        raise RuntimeError("Hatari/FreeMiNT run failed; evidence: " + str(out))

    image_bytes = image.read_bytes()
    result_lines = [name + "=PASS\r\n" for name in EXPECTED]
    missing = [line for line in result_lines if line.encode("ascii") not in image_bytes]
    if b"overall=PASS\r\n" not in image_bytes or missing:
        failed = [line.decode("ascii").strip() for line in missing]
        (out / "raw-result-check.txt").write_text(
            "overall=PASS marker present: " +
            str(b"overall=PASS\r\n" in image_bytes) + "\nmissing: " +
            ", ".join(failed) + "\n", encoding="ascii")
        raise RuntimeError("production cleanup probe failed: " + str(failed) +
                           "; evidence: " + str(out))
    (out / "result.txt").write_text(
        "FreeMiNT production-source helper probe: PASS\n" +
        "\n".join(line.strip() for line in result_lines) +
        "\noverall=PASS\n", encoding="ascii")

    status = subprocess.check_output(["git", "status", "--short"], cwd=ROOT,
                                     text=True).splitlines()
    diff = subprocess.check_output(["git", "diff", "--binary"], cwd=ROOT)
    artifacts = ["diskroot/PROBE.PRG", "cleanup.img", "hconsole.txt",
                 "image-create.log", "hatari.log", "result.txt"]
    screenshot = out / "grab0001.png"
    if screenshot.is_file():
        artifacts.append(screenshot.name)
    record = {
        "purpose": "Exercise production Atari GUI staged-output cleanup on FreeMiNT",
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                             cwd=ROOT, text=True).strip(),
        "worktree_status": status,
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "environment": {
            "hatari": next((line.strip() for line in hatari_run.stdout.splitlines()
                            if line.startswith("INFO : Hatari v")), "version in hatari.log"),
            "rom_sha256": sha256(rom),
            "rom": rom.name,
            "ram_mib": 4,
            "disk": "8 MiB ACSI FAT16",
            "probe_prg_sha256": sha256(probe),
        },
        "input": "FreeMiNT INIT launches the test-only main from the production Atari GUI translation unit.",
        "result": "PASS: held parent/root handles remove the incomplete stage and checksummed journal; directory rename while pinned is refused; sentinel is unchanged.",
        "limitations": [
            "This runs production cleanup helpers through a test-only entry point, not the interactive GEM application.",
            "FreeMiNT refused the parent rename while handles were open; moved-parent cleanup was not exercised on this target.",
            "The FAT image result file was verified by its raw image bytes after Hatari exited.",
        ],
        "artifacts": {name: {"sha256": sha256(out / name),
                              "size": (out / name).stat().st_size}
                      for name in artifacts},
    }
    manifest = out / "run.json"
    manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("FreeMiNT production-source staged cleanup passed")
    print("Evidence: " + str(manifest))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.TimeoutExpired, subprocess.CalledProcessError,
            RuntimeError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        sys.exit(1)
