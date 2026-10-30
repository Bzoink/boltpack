#!/usr/bin/env python3
"""Test Atari GEM extraction rollback after the old target has been backed up."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(command, cwd, env=None, timeout=30):
    return subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", type=Path, required=True,
                        help="dedicated test build with post-backup publish failure enabled")
    parser.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    parser.add_argument("--expect-recovery", action="store_true",
                        help="expect a competing target to prevent rollback and keep the backup")
    args = parser.parse_args()
    hatari = shutil.which(args.hatari)
    if hatari is None:
        raise RuntimeError("Hatari executable not found: " + args.hatari)
    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    out_root = args.output_root.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    prefix = "atari-gui-recovery-" if args.expect_recovery else "atari-gui-rollback-"
    out = Path(tempfile.mkdtemp(prefix=prefix, dir=out_root))
    host = out / "host"
    host.mkdir()
    payload = b"new archive bytes that must not replace the old target\n" * 29
    original = b"existing destination must be restored after publish failure\n"
    (host / "PAYLOAD.TXT").write_bytes(payload)
    (host / "OUT").mkdir()
    target = host / "OUT" / "PAYLOAD.TXT"
    target.write_bytes(original)
    shutil.copy2(gui, host / "BOLTPACK.PRG")
    created = run([str(linux), "c", "ARCHIVE.BLT", "PAYLOAD.TXT"], host)
    (out / "archive-create.log").write_text(created.stdout, encoding="utf-8")
    if created.returncode != 0:
        raise RuntimeError("could not create rollback fixture: " + str(out))
    script = out / "HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 4",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 3", "text OUT", "keypress 28",
        "sleep 15", "screenshot",
        "keypress 28", "sleep 2", "screenshot",
        "sleep 1", "quit", "",
    )), encoding="ascii")
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    env["PATH"] = str(Path(hatari).resolve().parent) + os.pathsep + env.get("PATH", "")
    command = [sys.executable, str(hconsole), str(script), "--exit", "--",
               "--machine", "ste", "--tos", str(tos), "--sound", "off",
               "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
               "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG"]
    completed = run(command, out, env=env, timeout=180)
    (out / "hatari.log").write_text(completed.stdout, encoding="utf-8")
    backups = sorted(path for path in (host / "OUT").iterdir()
                     if path.name.startswith("BR") and len(path.name) == 8 and
                     "." not in path.name)
    stages = sorted(path for path in (host / "OUT").iterdir()
                    if path.name.startswith("BP") and len(path.name) == 8 and
                    "." not in path.name)
    if args.expect_recovery:
        expected_blocker = b"test-only rollback blocker\n"
        if (completed.returncode != 0 or target.read_bytes() != expected_blocker or
                len(backups) != 1 or backups[0].read_bytes() != original or stages or
                not (out / "grab0001.png").is_file()):
            raise RuntimeError("rollback failure did not retain the original at a recovery path: " + str(out))
        recovery_path = str(backups[0].relative_to(host))
    elif (completed.returncode != 0 or target.read_bytes() != original or backups or stages or
          not (out / "grab0001.png").is_file()):
        raise RuntimeError("post-backup failure did not restore the original cleanly: " + str(out))
    else:
        recovery_path = None
    revision = run(["git", "rev-parse", "HEAD"], ROOT)
    status = run(["git", "status", "--short"], ROOT)
    diff = run(["git", "diff", "--binary", "--", "Makefile",
                "src/frontend/atari_gui/main.c",
                "test/check_hatari_gui_rollback.py"], ROOT)
    if revision.returncode != 0 or status.returncode != 0 or diff.returncode != 0:
        raise RuntimeError("could not capture source identity for rollback evidence: " + str(out))
    (out / "source-diff.patch").write_text(diff.stdout, encoding="utf-8")
    (out / "source-status.txt").write_text(status.stdout, encoding="utf-8")
    result = {
        "purpose": ("verify user-visible recovery after a competing target prevents rollback" if args.expect_recovery else
                    "verify production rollback after moving an existing extraction target to backup"),
        "injected_failure": ("test-only failure of stage-to-target publication plus a competing target before rollback" if args.expect_recovery else
                             "test-only failure of stage-to-target publication after backup verification"),
        "hatari": run([hatari, "--version"], out).stdout.strip(),
        "tos_rom": tos.name,
        "tos_sha256": hashlib.sha256(tos.read_bytes()).hexdigest(),
        "gui_sha256": hashlib.sha256(gui.read_bytes()).hexdigest(),
        "frontend_source_sha256": hashlib.sha256(
            (ROOT / "src/frontend/atari_gui/main.c").read_bytes()).hexdigest(),
        "makefile_sha256": hashlib.sha256((ROOT / "Makefile").read_bytes()).hexdigest(),
        "source_revision": revision.stdout.strip(),
        "source_status_sha256": hashlib.sha256(status.stdout.encode("utf-8")).hexdigest(),
        "source_diff_sha256": hashlib.sha256(diff.stdout.encode("utf-8")).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "archive_sha256": hashlib.sha256((host / "ARCHIVE.BLT").read_bytes()).hexdigest(),
        "original_target_sha256": hashlib.sha256(original).hexdigest(),
        "observed_target_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        "recovery_path": recovery_path,
        "retained_backup_sha256": hashlib.sha256(backups[0].read_bytes()).hexdigest() if backups else None,
        "unpublished_stage_names": [path.name for path in stages],
        "recovery_screenshot": "grab0001.png" if args.expect_recovery else None,
        "result": ("PASS: original remains byte-for-byte at reported recovery path; stage cleaned" if args.expect_recovery else
                   "PASS: previous target restored byte-for-byte and stage/backup cleaned"),
    }
    (out / "run.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.expect_recovery:
        print("PASS: Atari GEM reported and retained the original at " + recovery_path +
              "; evidence: " + str(out))
    else:
        print("PASS: Atari GEM post-backup rollback preserved original and cleaned temporary names; evidence: " + str(out))


if __name__ == "__main__":
    main()
