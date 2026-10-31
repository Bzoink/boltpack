#!/usr/bin/env python3
"""Exercise DJGPP extraction refusal after a DOSBox-X FAT12 parent swap."""
import argparse
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "test/reference/interoperability/dos-djgpp-parent-race.json"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_version(command):
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False,
                                timeout=15)
        return result.stdout.decode("utf-8", "replace").strip().splitlines()[:4]
    except (OSError, subprocess.TimeoutExpired) as exc:
        return ["unavailable: " + str(exc)]


def run(command, **kwargs):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs).stdout


def source_snapshot():
    rev = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                         check=True, stdout=subprocess.PIPE,
                         text=True).stdout.strip()
    status = subprocess.run(
        ["git", "-C", str(ROOT), "status", "--porcelain=v1",
         "--untracked-files=all"], check=True, stdout=subprocess.PIPE,
        text=True).stdout
    diff = subprocess.run(["git", "-C", str(ROOT), "diff", "--binary", "HEAD"],
                          check=True, stdout=subprocess.PIPE).stdout
    return rev, status, diff


def artifact_index(directory, record):
    indexed = []
    for path in sorted(directory.rglob("*")):
        if path.is_file() and path != record:
            indexed.append({"path": str(path.relative_to(directory)),
                            "size": path.stat().st_size,
                            "sha256": sha256(path)})
    return indexed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dosbox-x", default="dosbox-x")
    parser.add_argument("--dos-cc", default="i586-pc-msdosdjgpp-gcc")
    parser.add_argument("--cwsdpmi", type=Path, required=True)
    parser.add_argument("--dos-exe", type=Path,
                        default=ROOT / "build/dos-djgpp-parent-race/boltpack-race.exe")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output", type=Path,
                        help="new directory for retained image, logs, and run record")
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    out = args.output.resolve() if args.output else Path(
        tempfile.mkdtemp(prefix="boltpack-dos-parent-race-", dir=ROOT / "build"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    dos_exe = args.dos_exe.resolve(strict=True)
    dpmi = args.cwsdpmi.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    image = out / "parent-race-fat12.img"
    source = out / "SRC.TXT"
    source_payload = manifest["archive_payload_utf8"].encode("utf-8")
    source.write_bytes(source_payload)
    original = out / "original-target.txt"
    original_payload = manifest["original_target_utf8"].encode("utf-8")
    original.write_bytes(original_payload)
    archive = out / "LINUX.BLT"
    linux_create = run([str(linux), "c", "LINUX.BLT", "SRC.TXT"], cwd=out)
    (out / "linux-create.log").write_bytes(linux_create)

    run(["mformat", "-i", str(image), "-C", "-f", "720", "::"])
    for host_path, guest_path in (
            (dos_exe, "RACE.EXE"), (dpmi, "CWSDPMI.EXE"),
            (archive, "LINUX.BLT")):
        run(["mcopy", "-i", str(image), str(host_path), "::/" + guest_path])
    run(["mmd", "-i", str(image), "::/RACEOUT"])
    run(["mcopy", "-i", str(image), str(original), "::/RACEOUT/SRC.TXT"])

    command = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch",
               "-c", "imgmount a " + str(image) + " -t floppy",
               "-c", "a:", "-c", "RACE x -f -o RACEOUT LINUX.BLT",
               "-c", "exit"]
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    result = "failed"
    error = None
    dos_output = b""
    try:
        try:
            dos_output = run(command, env=env, timeout=args.timeout, cwd=out)
        except subprocess.TimeoutExpired as exc:
            dos_output = exc.stdout or b""
            (out / "dosboxx.log").write_bytes(dos_output)
            raise RuntimeError("DOSBox-X exceeded the parent-race timeout") from exc
        (out / "dosboxx.log").write_bytes(dos_output)

        hook_marker = out / "race-hook-marker.txt"
        refusal_marker = out / "race-refusal-marker.txt"
        run(["mcopy", "-i", str(image), "::/RACEHOOK.TXT", str(hook_marker)])
        run(["mcopy", "-i", str(image), "::/RACEREF.TXT", str(refusal_marker)])
        if hook_marker.read_text(encoding="ascii").strip() != \
                "parent substituted after stage close":
            raise RuntimeError("parent substitution hook did not report completion")
        if refusal_marker.read_text(encoding="ascii").strip() != \
                "staged path identity changed; publication refused":
            raise RuntimeError("production publisher did not refuse the changed stage path")

        parked_original = out / "parked-original-target.txt"
        replacement_sentinel = out / "replacement-root-sentinel.txt"
        run(["mcopy", "-i", str(image), "::/RACEPARK/SRC.TXT",
             str(parked_original)])
        run(["mcopy", "-i", str(image), "::/RACEOUT/KEEP.TXT",
             str(replacement_sentinel)])
        if parked_original.read_bytes() != original_payload:
            raise RuntimeError("parent substitution changed the original target")
        if replacement_sentinel.read_bytes() != \
                manifest["replacement_root_sentinel_utf8"].encode("utf-8"):
            raise RuntimeError("parent substitution changed the replacement-root sentinel")

        unexpected_target = out / "unexpected-target.txt"
        target_probe = subprocess.run(
            ["mcopy", "-i", str(image), "::/RACEOUT/SRC.TXT",
             str(unexpected_target)], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False)
        if target_probe.returncode == 0:
            raise RuntimeError("a file was published into the replacement output root")

        listings = {}
        staged_files = {}
        for guest_dir, label in (("RACEPARK", "verified"),
                                 ("RACEOUT", "counterfeit")):
            listing = run(["mdir", "-i", str(image), "::/" + guest_dir]).decode(
                "utf-8", "replace")
            listings[label] = listing
            match = re.search(r"^\s*(BP[0-9A-F]{6})\s+TMP\b", listing,
                              re.MULTILINE | re.IGNORECASE)
            if match is None:
                raise RuntimeError(label + " stage was not retained in " + guest_dir)
            host_copy = out / (label + "-stage.txt")
            run(["mcopy", "-i", str(image),
                 "::/" + guest_dir + "/" + match.group(1) + ".TMP",
                 str(host_copy)])
            staged_files[label] = host_copy
        if staged_files["verified"].read_bytes() != source_payload:
            raise RuntimeError("verified stage was not preserved byte-for-byte")
        fake_payload = manifest["counterfeit_stage_utf8"].encode("utf-8")
        if staged_files["counterfeit"].read_bytes() != fake_payload:
            raise RuntimeError("counterfeit stage was modified or published")
        result = "passed_refused_parent_substitution"
    except Exception as exc:
        error = str(exc)
        if dos_output:
            (out / "dosboxx.log").write_bytes(dos_output)

    revision, status, diff_bytes = source_snapshot()
    (out / "source.status.txt").write_text(status, encoding="utf-8")
    (out / "source.diff.patch").write_bytes(diff_bytes)
    record_path = out / "run.json"
    record = {
        "purpose": manifest["case"],
        "result": result,
        "error": error,
        "source": {
            "revision": revision,
            "worktree_status": status,
            "tracked_diff_sha256": hashlib.sha256(diff_bytes).hexdigest(),
            "frontend_sha256": sha256(ROOT / "src/frontend/dos/main.c"),
            "test_runner_sha256": sha256(Path(__file__)),
            "makefile_sha256": sha256(ROOT / "Makefile"),
            "expected_manifest_sha256": sha256(MANIFEST),
        },
        "manifest": str(MANIFEST.relative_to(ROOT)),
        "manifest_sha256": sha256(MANIFEST),
        "runtime": {
            "dosbox_x": command_version([args.dosbox_x, "--version"]),
            "djgpp_compiler": command_version([args.dos_cc, "--version"]),
            "host_platform": platform.platform(),
            "python": platform.python_version(),
            "locale": locale.setlocale(locale.LC_ALL, None),
            "timezone": {"TZ": os.environ.get("TZ"), "tzname": list(time.tzname)},
        },
        "build": {
            "test_executable": str(dos_exe.relative_to(ROOT)),
            "test_executable_sha256": sha256(dos_exe),
            "linux_cli_sha256": sha256(linux),
            "cwsdpmi_sha256": sha256(dpmi),
            "compiler_flags": ["-ansi", "-pedantic", "-Wall", "-Wextra",
                               "-O2", "-ffunction-sections", "-fdata-sections",
                               "-U__STRICT_ANSI__", "-DBP_TEST_DOS_PARENT_SUBSTITUTION"],
        },
        "filesystem": {"type": "FAT12", "image_bytes": image.stat().st_size,
                       "image_sha256": sha256(image)},
        "commands": command,
        "command_exit_statuses": {"linux_archive_create": 0,
                                   "dosbox_x": 0},
        "observed": {
            "parent_substitution_hook_reached": (out / "race-hook-marker.txt").is_file(),
            "stage_identity_refusal_reached": (out / "race-refusal-marker.txt").is_file(),
            "replacement_target_published": (out / "unexpected-target.txt").exists(),
            "original_target_sha256": sha256(out / "parked-original-target.txt")
                if (out / "parked-original-target.txt").is_file() else None,
            "replacement_root_sentinel_sha256": sha256(out / "replacement-root-sentinel.txt")
                if (out / "replacement-root-sentinel.txt").is_file() else None,
            "verified_stage_sha256": sha256(out / "verified-stage.txt")
                if (out / "verified-stage.txt").is_file() else None,
            "counterfeit_stage_sha256": sha256(out / "counterfeit-stage.txt")
                if (out / "counterfeit-stage.txt").is_file() else None,
            "expected_source_payload_sha256": hashlib.sha256(source_payload).hexdigest(),
            "directory_listings": locals().get("listings", {}),
        },
    }
    record["artifacts"] = artifact_index(out, record_path)
    record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(record, indent=2))
    if result != "passed_refused_parent_substitution":
        raise SystemExit("DOS parent-substitution probe failed; evidence: " + str(out))


if __name__ == "__main__":
    main()
